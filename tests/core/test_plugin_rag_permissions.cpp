// test_plugin_rag_permissions.cpp — права RAG из манифеста (R3.1).
//
// Проверяется БЕЗ PluginManager и без LlamaPluginHost: реализация живёт в
// plugin_rag_api.cpp, а решение о праве — в чистой plugin_has_permission.
//
//   - plugin_has_permission: пустое право, отсутствие, наличие;
//   - `_ex` ОТКАЗЫВАЮТ без права (denied): поиск/info требуют rag_access,
//     индексация/удаление/очистка — rag_write, flush — только rag_access;
//   - с правом — работают;
//   - порядок отказов: disabled и unavailable РАНЬШЕ denied (плагин без
//     прав на выключенном RAG получает disabled, а не denied).
//
// Эмбеддинги — детерминированный n-gram фолбэк (недостижимый server_url),
// как в test_plugin_rag_api: без модели и без сети.

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
             ("llama_gui_rag_perm_test_" + std::to_string((long long)::getpid()));
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

json take_json(char* p) {
    if (!p) return json();
    json j = json::parse(p, nullptr, /*allow_exceptions=*/false);
    std::free(p);
    return j;
}

bool has_code(const json& j, const std::string& code) {
    return j.is_object() && j.value("ok", true) == false &&
           j.value("code", std::string()) == code;
}

/* Контекст с явными правами. По умолчанию оба false — «нет манифеста». */
RagCallContext ctx_for(RagRegistry& reg, bool access, bool write,
                       const std::string& plugin = "wp_coder") {
    RagCallContext ctx;
    ctx.registry = &reg;
    ctx.allowed = true;
    ctx.plugin_name = plugin;
    ctx.has_rag_access = access;
    ctx.has_rag_write = write;
    return ctx;
}

} // namespace

// ============================================================================
// plugin_has_permission
// ============================================================================

void test_permission_lookup() {
    const std::vector<std::string> perms = {"rag_access", "llm_access"};

    TEST_ASSERT_TRUE(plugin_has_permission(perms, "rag_access"));
    TEST_ASSERT_TRUE(plugin_has_permission(perms, "llm_access"));
    TEST_ASSERT_FALSE(plugin_has_permission(perms, "rag_write"));
    /* Пустое право — не «разреши всем»: это ошибка вызывающего. */
    TEST_ASSERT_FALSE(plugin_has_permission(perms, ""));
    /* Нет манифеста → нет прав. */
    TEST_ASSERT_FALSE(plugin_has_permission({}, "rag_access"));
}

// ============================================================================
// _ex отказывают без права
// ============================================================================

void test_search_requires_access() {
    const auto root = temp_root();
    RagRegistry reg(root.string(), fallback_embeddings());

    /* Есть write, нет access → поиск/info запрещены. */
    const RagCallContext no_access = ctx_for(reg, /*access=*/false, /*write=*/true);
    TEST_ASSERT_TRUE(has_code(take_json(rag_search_ex_impl(no_access, "", "q", 3, "")),
                              "denied"));
    TEST_ASSERT_TRUE(has_code(take_json(rag_scope_info_impl(no_access, "")), "denied"));
    /* Сообщение называет именно нужное право — плагин не гадает, чего не хватает. */
    const json denied = take_json(rag_search_ex_impl(no_access, "", "q", 3, ""));
    TEST_ASSERT(denied.value("error", std::string()).find("rag_access") !=
                std::string::npos);
}

void test_write_ops_require_write() {
    const auto root = temp_root();
    RagRegistry reg(root.string(), fallback_embeddings());

    const std::string doc = (root / "doc.txt").string();
    write_file(doc, "alpha unique marker\n");

    /* Есть access, нет write → изменяющие операции запрещены. */
    const RagCallContext no_write = ctx_for(reg, /*access=*/true, /*write=*/false);

    TEST_ASSERT_TRUE(has_code(
        take_json(rag_index_document_ex_impl(no_write, "", doc, "")), "denied"));
    TEST_ASSERT_TRUE(has_code(
        take_json(rag_scope_remove_document_impl(no_write, "", doc)), "denied"));
    /* clear/flush возвращают int: отказ = 0, а не «успех». */
    TEST_ASSERT_EQUAL(rag_scope_clear_impl(no_write, ""), 0);

    /* Индекс не тронут: отказ не должен был что-либо изменить. */
    const RagCallContext with_access = ctx_for(reg, /*access=*/true, /*write=*/false);
    TEST_ASSERT_EQUAL(
        take_json(rag_scope_info_impl(with_access, "")).value("chunks", 0), 0);
}

void test_flush_needs_only_access() {
    const auto root = temp_root();
    RagRegistry reg(root.string(), fallback_embeddings());

    const std::string doc = (root / "doc.txt").string();
    write_file(doc, "alpha unique marker\n");

    /* Проиндексировать может только write-плагин. */
    TEST_ASSERT_EQUAL(
        take_json(rag_index_document_ex_impl(ctx_for(reg, true, true), "", doc, ""))
            .value("chunks", 0),
        1);

    /* У плагина только rag_access: flush разрешён (это не изменение индекса,
     * а запись уже изменённого на диск). */
    const RagCallContext read_only = ctx_for(reg, /*access=*/true, /*write=*/false);
    TEST_ASSERT_EQUAL(rag_scope_flush_impl(read_only, ""), 1);
}

void test_with_permissions_all_work() {
    const auto root = temp_root();
    RagRegistry reg(root.string(), fallback_embeddings());

    const std::string doc = (root / "doc.txt").string();
    write_file(doc, "alpha unique marker for permitted plugin\n");

    const RagCallContext full = ctx_for(reg, /*access=*/true, /*write=*/true);
    TEST_ASSERT_EQUAL(
        take_json(rag_index_document_ex_impl(full, "", doc, "")).value("ok", false),
        true);
    TEST_ASSERT_EQUAL(
        take_json(rag_search_ex_impl(full, "", "unique marker", 5, ""))
            .value("ok", false),
        true);
    TEST_ASSERT_EQUAL(
        take_json(rag_scope_info_impl(full, "")).value("ok", false), true);
}

// ============================================================================
// Порядок отказов: disabled/unavailable раньше denied
// ============================================================================

void test_disabled_and_unavailable_beat_denied() {
    /* Выключенный RAG и плагин без прав: должен отвечать disabled, а не
     * denied — иначе плагин будет искать проблему в манифесте, а не в
     * настройках пользователя. */
    RagCallContext off;
    off.registry = nullptr;
    off.allowed = false;
    off.plugin_name = "wp_coder";
    off.has_rag_access = false;
    off.has_rag_write = false;
    TEST_ASSERT_TRUE(has_code(take_json(rag_search_ex_impl(off, "", "q", 3, "")),
                              "disabled"));

    /* Реестра нет, права есть — unavailable, а не denied. */
    RagCallContext no_reg;
    no_reg.registry = nullptr;
    no_reg.allowed = true;
    no_reg.plugin_name = "wp_coder";
    no_reg.has_rag_access = true;
    no_reg.has_rag_write = true;
    TEST_ASSERT_TRUE(has_code(take_json(rag_search_ex_impl(no_reg, "", "q", 3, "")),
                              "unavailable"));
}

int main() {
    REGISTER_TEST(test_permission_lookup);

    REGISTER_TEST(test_search_requires_access);
    REGISTER_TEST(test_write_ops_require_write);
    REGISTER_TEST(test_flush_needs_only_access);
    REGISTER_TEST(test_with_permissions_all_work);

    REGISTER_TEST(test_disabled_and_unavailable_beat_denied);

    const int rc = test::TestRunner::instance().run();

    std::error_code ec;
    std::filesystem::remove_all(
        std::filesystem::temp_directory_path() /
        ("llama_gui_rag_perm_test_" + std::to_string((long long)::getpid())),
        ec);
    return rc;
}