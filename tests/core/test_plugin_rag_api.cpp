// test_plugin_rag_api.cpp — ABI RAG v2: скоупы и JSON-контракт (R2.2).
//
// Проверяется БЕЗ PluginManager и без LlamaPluginHost: реализация живёт в
// plugin_rag_api.cpp именно для того, чтобы её можно было звать напрямую.
//
//   - resolve_scope: свой скоуп, свой скоуп с суффиксом, чужой скоуп,
//     попытка «прилипнуть» к чужому имени, `..`, пустой суффикс;
//   - порядок отказов: выключено → реестра нет → скоуп не твой (плагин
//     не должен искать ошибку в имени, когда выключены настройки);
//   - JSON-контракт: ok/code/error, поля чанка, поля info;
//   - изоляция через ABI: документ скоупа A не виден в B;
//   - удаление документа возвращает его в индекс (профиль чистится).
//
// Эмбеддинги — детерминированный n-gram фолбэк (недостижимый server_url),
// как в test_rag_scope_isolation: без модели и без сети.

#include "plugins/plugin_rag_api.h"

#include "../test_framework.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>

using namespace llama_gui::core;
using namespace llama_gui::plugin;
using json = nlohmann::json;

namespace {

std::filesystem::path temp_root() {
    auto p = std::filesystem::temp_directory_path() /
             ("llama_gui_rag_api_test_" + std::to_string((long long)::getpid()));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

void write_file(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::shared_ptr<EmbeddingGenerator> fallback_embeddings() {
    auto gen = std::make_shared<EmbeddingGenerator>();
    gen->set_server_url("http://127.0.0.1:9");   /* заведомо мёртвый адрес */
    if (!gen->load_model()) {
        std::cerr << "test: cannot prepare fallback embeddings" << std::endl;
        std::abort();
    }
    return gen;
}

/* Разобрать ответ и освободить malloc-строку (в ABI это free_string). */
json take_json(char* p) {
    if (!p) return json();
    json j = json::parse(p, nullptr, /*allow_exceptions=*/false);
    std::free(p);
    return j;
}

RagCallContext ctx_for(RagRegistry& reg, const std::string& plugin,
                       bool allowed = true) {
    RagCallContext ctx;
    ctx.registry = &reg;
    ctx.allowed = allowed;
    ctx.plugin_name = plugin;
    /* R3.1: `_ex`-функции требуют прав. Тесты R2 проверяют JSON-контракт
     * и изоляцию, а не права, поэтому контекст по умолчанию наделяется
     * обоими. Без RAG-прав здесь все `_ex` отвечали бы `denied`. */
    ctx.has_rag_access = true;
    ctx.has_rag_write = true;
    return ctx;
}

bool has_code(const json& j, const std::string& code) {
    return j.is_object() && j.value("ok", true) == false &&
           j.value("code", std::string()) == code;
}

} // namespace

// ============================================================================
// resolve_scope
// ============================================================================

void test_resolve_scope_default_and_own() {
    std::string suffix = "не тронут";
    std::string error;

    TEST_ASSERT_TRUE(resolve_scope("wp_coder", "", suffix, error));
    TEST_ASSERT_EQUAL(suffix, std::string());

    TEST_ASSERT_TRUE(resolve_scope("wp_coder", "plugin:wp_coder", suffix, error));
    TEST_ASSERT_EQUAL(suffix, std::string());

    TEST_ASSERT_TRUE(resolve_scope("wp_coder", "plugin:wp_coder:archive",
                                   suffix, error));
    TEST_ASSERT_EQUAL(suffix, std::string("archive"));

    /* Точка в суффиксе разрешена (домен: site.example.com). */
    TEST_ASSERT_TRUE(resolve_scope("wp_coder", "plugin:wp_coder:site.example.com",
                                   suffix, error));
    TEST_ASSERT_EQUAL(suffix, std::string("site.example.com"));
}

void test_resolve_scope_rejects_foreign_and_broken() {
    std::string suffix;
    std::string error;

    /* Чужой плагин. */
    TEST_ASSERT_FALSE(resolve_scope("wp_coder", "plugin:siteparser", suffix, error));
    TEST_ASSERT_FALSE(error.empty());

    /* «Прилипание» к чужому имени: plugin:wp_coder_extra — не суффикс, а
     * другой плагин с похожим именем. */
    TEST_ASSERT_FALSE(resolve_scope("wp_coder", "plugin:wp_coder_extra", suffix, error));

    /* Выход из каталога. */
    TEST_ASSERT_FALSE(resolve_scope("wp_coder", "../../etc", suffix, error));
    TEST_ASSERT_FALSE(resolve_scope("wp_coder", "plugin:wp_coder:../x", suffix, error));

    /* Пустой суффикс после двоеточия — опечатка, а не «по умолчанию». */
    TEST_ASSERT_FALSE(resolve_scope("wp_coder", "plugin:wp_coder:", suffix, error));

    /* Невалидное имя самого плагина (хост такого не пропустит, но
     * проверка должна быть и здесь: имя приходит из PluginInfo). */
    TEST_ASSERT_FALSE(resolve_scope("../etc", "", suffix, error));
}

// ============================================================================
// Порядок отказов
// ============================================================================

void test_ex_respects_disabled_before_scope() {
    const auto root = temp_root();
    RagRegistry reg(root.string(), fallback_embeddings());
    const RagCallContext ctx = ctx_for(reg, "wp_coder", /*allowed=*/false);

    /* Выключенный RAG отвечает disabled ДАЖЕ на заведомо чужой скоуп:
     * иначе плагин по коду "bad_scope" решил бы, что дело в имени. */
    const json j = take_json(rag_search_ex_impl(ctx, "plugin:siteparser", "q", 3, ""));
    TEST_ASSERT_TRUE(has_code(j, "disabled"));
}

void test_ex_unavailable_without_registry() {
    RagCallContext ctx;
    ctx.registry = nullptr;
    ctx.allowed = true;
    ctx.plugin_name = "wp_coder";

    const json j = take_json(rag_scope_info_impl(ctx, ""));
    TEST_ASSERT_TRUE(has_code(j, "unavailable"));
}

void test_ex_bad_scope_is_reported() {
    const auto root = temp_root();
    RagRegistry reg(root.string(), fallback_embeddings());
    const RagCallContext ctx = ctx_for(reg, "wp_coder");

    const json j = take_json(rag_search_ex_impl(ctx, "plugin:siteparser", "q", 3, ""));
    TEST_ASSERT_TRUE(has_code(j, "bad_scope"));
    TEST_ASSERT_FALSE(j.value("error", std::string()).empty());
}

// ============================================================================
// JSON-контракт
// ============================================================================

void test_index_then_search_json() {
    const auto root = temp_root();
    RagRegistry reg(root.string(), fallback_embeddings());
    const RagCallContext ctx = ctx_for(reg, "wp_coder");

    const std::string doc = (root / "doc.txt").string();
    const std::string text = "alpha unique marker for wp_coder scope";
    write_file(doc, text + "\n");

    const json idx = take_json(
        rag_index_document_ex_impl(ctx, "", doc, /*opts_json=*/""));
    TEST_ASSERT_EQUAL(idx.value("ok", false), true);
    TEST_ASSERT_EQUAL(idx.value("chunks", 0), 1);
    TEST_ASSERT_EQUAL(idx.value("scope", std::string()), std::string("plugin:wp_coder"));

    const json res = take_json(rag_search_ex_impl(ctx, "", text, 5, ""));
    TEST_ASSERT_EQUAL(res.value("ok", false), true);
    TEST_ASSERT(res.contains("chunks"));
    TEST_ASSERT(res["chunks"].is_array());
    TEST_ASSERT(!res["chunks"].empty());

    const json chunk = res["chunks"][0];
    for (const char* field : {"content", "document_id", "chunk_index",
                              "file_path", "symbol_name", "start_line",
                              "end_line"}) {
        TEST_ASSERT(chunk.contains(field));
    }
    TEST_ASSERT_EQUAL(chunk.value("file_path", std::string()), doc);
}

void test_bad_request_codes() {
    const auto root = temp_root();
    RagRegistry reg(root.string(), fallback_embeddings());
    const RagCallContext ctx = ctx_for(reg, "wp_coder");

    TEST_ASSERT_TRUE(has_code(take_json(rag_search_ex_impl(ctx, "", "", 5, "")),
                              "bad_request"));
    TEST_ASSERT_TRUE(has_code(take_json(rag_search_ex_impl(ctx, "", "q", 0, "")),
                              "bad_request"));
    TEST_ASSERT_TRUE(has_code(take_json(rag_index_document_ex_impl(ctx, "", "", "")),
                              "bad_request"));
    TEST_ASSERT_TRUE(has_code(take_json(rag_scope_remove_document_impl(ctx, "", "")),
                              "bad_request"));
}

void test_info_reports_unavailability_honestly() {
    const auto root = temp_root();
    /* Реестр БЕЗ эмбеддингов: index_document вернёт 0, а info обязан
     * сказать «нет эмбеддингов», а не «индекс пуст». */
    RagRegistry reg(root.string(), nullptr);
    const RagCallContext ctx = ctx_for(reg, "wp_coder");

    const json info = take_json(rag_scope_info_impl(ctx, ""));
    TEST_ASSERT_EQUAL(info.value("ok", false), true);
    TEST_ASSERT_EQUAL(info.value("embeddings", true), false);
    TEST_ASSERT_EQUAL(info.value("ready", true), false);
    TEST_ASSERT_FALSE(info.value("reason", std::string()).empty());
}

// ============================================================================
// Изоляция и жизненный цикл через ABI
// ============================================================================

void test_two_plugins_do_not_see_each_other() {
    const auto root = temp_root();
    RagRegistry reg(root.string(), fallback_embeddings());

    const std::string doc_a = (root / "a.txt").string();
    const std::string doc_b = (root / "b.txt").string();
    write_file(doc_a, "alpha unique marker\n");
    write_file(doc_b, "beta unique marker\n");

    TEST_ASSERT_EQUAL(
        take_json(rag_index_document_ex_impl(ctx_for(reg, "plugin_a"), "", doc_a, ""))
            .value("chunks", 0),
        1);
    TEST_ASSERT_EQUAL(
        take_json(rag_index_document_ex_impl(ctx_for(reg, "plugin_b"), "", doc_b, ""))
            .value("chunks", 0),
        1);

    /* B не видит документ A: в его выдаче только b.txt. */
    const json in_b = take_json(
        rag_search_ex_impl(ctx_for(reg, "plugin_b"), "", "unique marker", 10, ""));
    TEST_ASSERT_EQUAL(in_b.value("ok", false), true);
    for (const auto& chunk : in_b["chunks"]) {
        TEST_ASSERT_EQUAL(chunk.value("file_path", std::string()), doc_b);
    }

    /* Счётчики независимы. */
    const json info_a = take_json(rag_scope_info_impl(ctx_for(reg, "plugin_a"), ""));
    const json info_b = take_json(rag_scope_info_impl(ctx_for(reg, "plugin_b"), ""));
    TEST_ASSERT_EQUAL(info_a.value("chunks", 0), 1);
    TEST_ASSERT_EQUAL(info_b.value("chunks", 0), 1);
}

void test_remove_document_lets_index_again() {
    const auto root = temp_root();
    RagRegistry reg(root.string(), fallback_embeddings());
    const RagCallContext ctx = ctx_for(reg, "wp_coder");

    const std::string doc = (root / "doc.txt").string();
    write_file(doc, "alpha unique marker\n");

    TEST_ASSERT_EQUAL(
        take_json(rag_index_document_ex_impl(ctx, "", doc, "")).value("chunks", 0), 1);

    const json removed = take_json(rag_scope_remove_document_impl(ctx, "", doc));
    TEST_ASSERT_EQUAL(removed.value("ok", false), true);
    TEST_ASSERT_EQUAL(removed.value("removed", false), true);

    /* Повторное удаление — не ошибка (идемпотентность). */
    const json removed_again = take_json(rag_scope_remove_document_impl(ctx, "", doc));
    TEST_ASSERT_EQUAL(removed_again.value("ok", false), true);
    TEST_ASSERT_EQUAL(removed_again.value("removed", false), false);

    /* Документ ушёл из индекса. */
    const json after = take_json(rag_scope_info_impl(ctx, ""));
    TEST_ASSERT_EQUAL(after.value("chunks", 0), 0);

    /* И его можно вернуть: профиль тоже очищен, иначе дедупликация R1.6
     * сочла бы документ «уже проиндексированным». */
    TEST_ASSERT_EQUAL(
        take_json(rag_index_document_ex_impl(ctx, "", doc, "")).value("chunks", 0), 1);
}

void test_clear_and_flush_return_status() {
    const auto root = temp_root();
    RagRegistry reg(root.string(), fallback_embeddings());
    const RagCallContext ctx = ctx_for(reg, "wp_coder");

    const std::string doc = (root / "doc.txt").string();
    write_file(doc, "alpha unique marker\n");
    TEST_ASSERT_EQUAL(
        take_json(rag_index_document_ex_impl(ctx, "", doc, "")).value("chunks", 0), 1);

    /* Без общего генератора флаш писать нечего, но у стора он есть. */
    TEST_ASSERT_EQUAL(rag_scope_flush_impl(ctx, ""), 1);
    TEST_ASSERT_EQUAL(rag_scope_clear_impl(ctx, ""), 1);
    TEST_ASSERT_EQUAL(take_json(rag_scope_info_impl(ctx, ""))
                          .value("chunks", 0),
                      0);
}

// ============================================================================
// R4.1: суффикс скоупа из настроек (scope_suffix)
// ============================================================================

void test_resolve_scope_appends_extra_suffix() {
    std::string suffix;
    std::string error;

    /* Пустой запрошенный суффикс + extra → extra. */
    TEST_ASSERT_TRUE(resolve_scope("wp_coder", "", suffix, error, "s1"));
    TEST_ASSERT_EQUAL(suffix, std::string("s1"));

    /* Запрошенный "archive" + extra "s1" → "archives1": дописывается. */
    TEST_ASSERT_TRUE(resolve_scope("wp_coder", "plugin:wp_coder:archive",
                                   suffix, error, "s1"));
    TEST_ASSERT_EQUAL(suffix, std::string("archives1"));

    /* Без extra поведение прежнее. */
    TEST_ASSERT_TRUE(resolve_scope("wp_coder", "plugin:wp_coder:archive",
                                   suffix, error));
    TEST_ASSERT_EQUAL(suffix, std::string("archive"));

    /* Невалидный extra нормализуется вместе со скоупом → bad_scope, а не
     * запись «в никуда». */
    TEST_ASSERT_FALSE(resolve_scope("wp_coder", "", suffix, error, "../x"));
    TEST_ASSERT_FALSE(error.empty());
}

/* «Живой» пример через РЕАЛЬНЫЙ хостовый RAG (RagRegistry + RagStore +
 * ABI _impl, без моков): суффикс из настроек реально дописывается к скоупу
 * и изолирует индекс. Это тот же путь, которым ходит archive_write/
 * archive_search плагина. */
void test_scope_suffix_isolates_index() {
    const auto root = temp_root();
    RagRegistry reg(root.string(), fallback_embeddings());

    const std::string doc = (root / "doc.txt").string();
    const std::string text = "beta unique marker for scope suffix";
    write_file(doc, text + "\n");

    /* Индексируем в "plugin:wp_coder:archive" с суффиксом "s1" → "archives1". */
    RagCallContext ctx = ctx_for(reg, "wp_coder");
    ctx.scope_suffix = "s1";
    const json idx = take_json(
        rag_index_document_ex_impl(ctx, "plugin:wp_coder:archive", doc, ""));
    TEST_ASSERT_EQUAL(idx.value("ok", false), true);
    TEST_ASSERT_EQUAL(idx.value("scope", std::string()),
                      std::string("plugin:wp_coder:archives1"));

    /* Поиск с тем же суффиксом — находит. */
    const json hit = take_json(
        rag_search_ex_impl(ctx, "plugin:wp_coder:archive", text, 5, ""));
    TEST_ASSERT_EQUAL(hit.value("ok", false), true);
    TEST_ASSERT(hit.contains("chunks"));
    TEST_ASSERT(!hit["chunks"].empty());

    /* Поиск БЕЗ суффикса — другой скоуп, ничего не видит (изоляция). */
    RagCallContext no_suffix = ctx_for(reg, "wp_coder");
    const json miss = take_json(
        rag_search_ex_impl(no_suffix, "plugin:wp_coder:archive", text, 5, ""));
    TEST_ASSERT_EQUAL(miss.value("ok", false), true);
    TEST_ASSERT(miss["chunks"].empty());

    /* Инфо подтверждает имя скоупа с суффиксом. */
    const json info =
        take_json(rag_scope_info_impl(ctx, "plugin:wp_coder:archive"));
    TEST_ASSERT_EQUAL(info.value("scope", std::string()),
                      std::string("plugin:wp_coder:archives1"));
}

int main() {
    REGISTER_TEST(test_resolve_scope_default_and_own);
    REGISTER_TEST(test_resolve_scope_rejects_foreign_and_broken);

    REGISTER_TEST(test_ex_respects_disabled_before_scope);
    REGISTER_TEST(test_ex_unavailable_without_registry);
    REGISTER_TEST(test_ex_bad_scope_is_reported);

    REGISTER_TEST(test_index_then_search_json);
    REGISTER_TEST(test_bad_request_codes);
    REGISTER_TEST(test_info_reports_unavailability_honestly);

    REGISTER_TEST(test_two_plugins_do_not_see_each_other);
    REGISTER_TEST(test_remove_document_lets_index_again);
    REGISTER_TEST(test_clear_and_flush_return_status);
    REGISTER_TEST(test_resolve_scope_appends_extra_suffix);
    REGISTER_TEST(test_scope_suffix_isolates_index);

    const int rc = test::TestRunner::instance().run();

    std::error_code ec;
    std::filesystem::remove_all(
        std::filesystem::temp_directory_path() /
        ("llama_gui_rag_api_test_" + std::to_string((long long)::getpid())),
        ec);
    return rc;
}