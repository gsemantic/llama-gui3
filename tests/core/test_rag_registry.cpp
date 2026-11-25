// test_rag_registry.cpp — реестр скоупов RAG (R1.2).
//
// Проверяется БЕЗ FAISS и без эмбеддингов: реестр и его чистые функции
// (нормализация имени, путь, разбор квот) не должны требовать ни модели,
// ни индекса. Это ровно та граница, где раньше была дыра: плагин мог
// назваться `../..`, и путь уходил за пределы каталога индексов.
//
// Что проверяется:
//   - normalize_scope: валидные имена, враждебные (`../`, `a/b`, пусто,
//     заглавные, не-ASCII), суффиксы;
//   - scope_dir: путь внутри root, отказ на выходе наружу, стабильность
//     имени (двоеточие/точка → подчёркивание);
//   - parse_scope_options: отсутствующие поля, мусор, отрицательные квоты,
//     read_only → allow_index=false;
//   - RagRegistry: lazy-создание, изоляция (два плагина → два разных
//     каталога), list/remove/loaded_count, LRU-вытеснение.

#include "../../include/core/rag_registry.h"
#include "../../include/core/rag_store.h"
#include "../test_framework.h"

#include <atomic>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace llama_gui::core;

namespace {

std::filesystem::path temp_root() {
    auto p = std::filesystem::temp_directory_path() /
             ("llama_gui_rag_registry_test_" + std::to_string((long long)::getpid()));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/* Детерминированные эмбеддинги без модели/сети (n-gram фолбэк). Нужны
 * только тесту LRU-флаша: без эмбеддингов чанки не создаются и «флаш»
 * проверять не на чем. */
std::shared_ptr<EmbeddingGenerator> fallback_embeddings() {
    auto gen = std::make_shared<EmbeddingGenerator>();
    gen->set_server_url("http://127.0.0.1:9");
    if (!gen->load_model()) {
        std::cerr << "test: cannot prepare fallback embeddings" << std::endl;
        std::abort();
    }
    return gen;
}

} // namespace

// ============================================================================
// normalize_scope
// ============================================================================

void test_scope_valid_plugin_name() {
    std::string scope;
    TEST_ASSERT_TRUE(normalize_scope("wp_coder", "", scope));
    TEST_ASSERT_EQUAL(scope, "plugin:wp_coder");

    TEST_ASSERT_TRUE(normalize_scope("a", "", scope));
    TEST_ASSERT_EQUAL(scope, "plugin:a");

    TEST_ASSERT_TRUE(normalize_scope("rag-agent", "", scope));
    TEST_ASSERT_EQUAL(scope, "plugin:rag-agent");
}

void test_scope_rejects_bad_plugin_names() {
    std::string scope;
    std::string error;
    const char* bad[] = {
        "",            /* пусто */
        "../etc",      /* выход вверх */
        "wp/coder",    /* слэш */
        "wp coder",    /* пробел */
        "WpCoder",     /* заглавные */
        "1wp",         /* цифра первой */
        "_wp",         /* подчёркивание первым */
        "wp.coder",    /* точка */
    };
    for (const char* n : bad) {
        TEST_ASSERT_FALSE(normalize_scope(n, "", scope, &error));
        TEST_ASSERT_FALSE(error.empty());
    }
}

void test_scope_rejects_bad_suffix() {
    std::string scope;
    TEST_ASSERT_TRUE(normalize_scope("wp_coder", "archive", scope));
    TEST_ASSERT_EQUAL(scope, "plugin:wp_coder:archive");

    TEST_ASSERT_TRUE(normalize_scope("wp_coder", "site-example.com", scope));
    TEST_ASSERT_EQUAL(scope, "plugin:wp_coder:site-example.com");

    /* Двоеточие в суффиксе запрещено: это разделитель скоупа. */
    TEST_ASSERT_FALSE(normalize_scope("wp_coder", "a:b", scope));
    /* Путь тоже. */
    TEST_ASSERT_FALSE(normalize_scope("wp_coder", "../archive", scope));
}

void test_scope_long_name_rejected() {
    std::string scope;
    const std::string name65(65, 'a');
    TEST_ASSERT_FALSE(normalize_scope(name65, "", scope));

    const std::string name64(64, 'a');
    TEST_ASSERT_TRUE(normalize_scope(name64, "", scope));
}

// ============================================================================
// scope_dir
// ============================================================================

void test_scope_dir_inside_root() {
    const std::string root = "/tmp/opencode/rag_root";
    const std::string dir = scope_dir(root, "plugin:wp_coder");
    TEST_ASSERT_FALSE(dir.empty());
    TEST_ASSERT(dir.rfind(root, 0) == 0);
    /* Двоеточие не должно попасть в имя каталога: на Windows это
     * недопустимый символ, а на Linux создаёт ложное «расширение». */
    TEST_ASSERT(dir.find(':') == std::string::npos);
    TEST_ASSERT(dir.find("plugin_wp_coder") != std::string::npos);
}

void test_scope_dir_rejects_empty() {
    TEST_ASSERT_EQUAL(scope_dir("", "plugin:x"), "");
    TEST_ASSERT_EQUAL(scope_dir("/tmp/x", ""), "");
}

void test_scope_dir_rejects_traversal() {
    /* Прямой вызов, МИМО normalize_scope.
 *
 * ЧТО ЭТОТ ТЕСТ УСТАНОВИЛ (важно, иначе он читается как «ловит exit»).
 * Мутация «снять проверку внутри root» НЕ ЛОМАЕТ НИ ОДИН из этих
 * случаев: все посторонние символы (`.`, `/`, `\`) заменяются на `_` при
 * построении имени каталога, поэтому `plugin:../../etc` становится
 * `plugin_____etc` и физически остаётся внутри root. Проверка
 * «внутри root» — структурно недостижимая страховка на случай, если
 * root сам пуст/подменён; она оставлена, но этот тест фиксирует
 * ФАКТИЧЕСКОЕ поведение (имя нормализуется), а не её работу.
 *
 * Настоящая проверка выхода наружу — normalize_scope (выше), и её
 * мутация ловится. */
    TEST_ASSERT_EQUAL(scope_dir("/tmp/rag_root", "../etc"), "");
    /* Точка в имени каталога запрещена (`is_name_char`), поэтому такой
     * скоуп отбрасывается целиком, а не превращается в harmless имя. */
    TEST_ASSERT_EQUAL(scope_dir("/tmp/rag_root", "plugin:../../etc"), "");
    TEST_ASSERT_EQUAL(scope_dir("/tmp/rag_root", "plugin:a/b"), "");
    TEST_ASSERT_EQUAL(scope_dir("/tmp/rag_root", "plugin:a\\b"), "");
    /* ДОКУМЕНТИРОВАННАЯ ЭКВИВАЛЕНТНОСТЬ (проверено мутацией): снять
     * проверку «результат внутри root» — и ни один тест не упадёт.
     * Причина не в слабости проверки: имя каталога строится только из
     * `[a-z0-9_-]`, поэтому выйти наружу через имя нельзя by design.
     * Проверка оставлена как последняя линия (root="" → иначе путь
     * стал бы относительным), но полагаться на неё как на «защиту от
     * traversal» нельзя: настоящая защита — `normalize_scope` выше. */
}

void test_scope_dir_stable() {
    /* Одно и то же имя → один и тот же путь: иначе повторное обращение
     * за тем же сторо создало бы новый индекс. */
    TEST_ASSERT_EQUAL(scope_dir("/tmp/r", "plugin:a:b"),
                      scope_dir("/tmp/r", "plugin:a:b"));
}

// ============================================================================
// parse_scope_options
// ============================================================================

void test_options_empty_json_is_defaults() {
    RagScopeOptions o;
    TEST_ASSERT_TRUE(parse_scope_options("", o));
    TEST_ASSERT_EQUAL(o.max_documents, 0);
    TEST_ASSERT_TRUE(o.allow_index);
    TEST_ASSERT_FALSE(o.read_only);
}

void test_options_reads_fields() {
    RagScopeOptions o;
    const std::string json =
        "{\"max_documents\":100,\"max_index_bytes\":1048576,"
        "\"allow_index\":false}";
    TEST_ASSERT_TRUE(parse_scope_options(json, o));
    TEST_ASSERT_EQUAL(o.max_documents, 100);
    TEST_ASSERT(o.max_index_bytes == 1048576LL);
    TEST_ASSERT_FALSE(o.allow_index);
}

void test_options_read_only_forbids_indexing() {
    RagScopeOptions o;
    TEST_ASSERT_TRUE(parse_scope_options("{\"read_only\":true}", o));
    TEST_ASSERT_TRUE(o.read_only);
    TEST_ASSERT_FALSE(o.allow_index);
}

void test_options_negative_clamped_to_zero() {
    RagScopeOptions o;
    TEST_ASSERT_TRUE(parse_scope_options("{\"max_documents\":-5}", o));
    /* -5 не должно стать «без лимита» наоборот или отрицательным
     * лимитом, который ломал бы сравнения. */
    TEST_ASSERT_EQUAL(o.max_documents, 0);
}

void test_options_bad_json_is_error_but_not_crash() {
    RagScopeOptions o;
    std::string error;
    TEST_ASSERT_FALSE(parse_scope_options("not json at all", o, &error));
    TEST_ASSERT_FALSE(error.empty());
}

void test_options_bad_field_type_ignored() {
    /* Опечатка в профиле не должна ломать доступ: поле игнорируется,
 * значение остаётся по умолчанию. */
    RagScopeOptions o;
    const std::string json = "{\"max_documents\":\"сто\"}";
    TEST_ASSERT_TRUE(parse_scope_options(json, o));
    TEST_ASSERT_EQUAL(o.max_documents, 0);
}

// --- R4.1: опции одного вызова индексации ---

void test_options_reads_index_call_fields() {
    RagScopeOptions o;
    const std::string json =
        "{\"max_bytes\":524288,\"max_chunks\":50,"
        "\"deadline_ms\":1500,\"reindex\":true}";
    TEST_ASSERT_TRUE(parse_scope_options(json, o));
    TEST_ASSERT(o.max_bytes == 524288LL);
    TEST_ASSERT_EQUAL(o.max_chunks, 50);
    TEST_ASSERT(o.deadline_ms == 1500LL);
    TEST_ASSERT_TRUE(o.reindex);
}

void test_options_index_fields_zero_means_scope_default() {
    /* 0 — «как в настройках скоупа», а не «ноль байт/чанков». */
    RagScopeOptions o;
    TEST_ASSERT_TRUE(parse_scope_options("{\"max_bytes\":0,\"max_chunks\":0}", o));
    TEST_ASSERT(o.max_bytes == 0LL);
    TEST_ASSERT_EQUAL(o.max_chunks, 0);
    TEST_ASSERT(o.deadline_ms == 0LL);
    TEST_ASSERT_FALSE(o.reindex);
}

void test_options_index_fields_clamped() {
    RagScopeOptions o;
    /* Отрицательные → 0 (а не «без лимита» наоборот). */
    TEST_ASSERT_TRUE(parse_scope_options(
        "{\"max_bytes\":-1,\"max_chunks\":-5,\"deadline_ms\":-9}", o));
    TEST_ASSERT(o.max_bytes == 0LL);
    TEST_ASSERT_EQUAL(o.max_chunks, 0);
    TEST_ASSERT(o.deadline_ms == 0LL);

    /* Сверх разумного предела — кламп, а не OOM/бесконечность. */
    RagScopeOptions big;
    TEST_ASSERT_TRUE(parse_scope_options(
        "{\"max_bytes\":1099511627776,\"max_chunks\":1000000,"
        "\"deadline_ms\":999999999999}", big));
    TEST_ASSERT(big.max_bytes == (1LL << 30));   /* 1 ГиБ */
    TEST_ASSERT_EQUAL(big.max_chunks, 100000);
    TEST_ASSERT(big.deadline_ms == 24LL * 60 * 60 * 1000);  /* 24 ч */
}

void test_options_index_bad_type_ignored() {
    RagScopeOptions o;
    TEST_ASSERT_TRUE(parse_scope_options("{\"max_chunks\":\"много\"}", o));
    TEST_ASSERT_EQUAL(o.max_chunks, 0);
}

// ============================================================================
// RagRegistry
// ============================================================================

void test_registry_get_creates_store() {
    const auto root = temp_root();
    RagRegistry reg(root.string());

    auto store = reg.get("wp_coder");
    TEST_ASSERT(store != nullptr);
    TEST_ASSERT_EQUAL(store->scope(), "plugin:wp_coder");

    const RagStoreInfo info = store->info();
    TEST_ASSERT_EQUAL(info.scope, "plugin:wp_coder");
    TEST_ASSERT(info.path.find("plugin_wp_coder") != std::string::npos);
    /* Без общего генератора эмбеддингов нет — стор обязан сказать это
 * прямо, а не притвориться готовым. */
    TEST_ASSERT_FALSE(info.embeddings);
    TEST_ASSERT_FALSE(info.ready);
}

void test_registry_rejects_bad_name() {
    const auto root = temp_root();
    RagRegistry reg(root.string());
    std::string error;
    TEST_ASSERT(reg.get("../etc", "", &error) == nullptr);
    TEST_ASSERT_FALSE(error.empty());
}

void test_registry_two_plugins_are_isolated() {
    /* Главное свойство R1: у двух плагинов РАЗНЫЕ каталоги, и запись
 * одного не видна другому. Проверяем каталогами, а не содержимым:
 * без эмбеддингов индексация всё равно не создаст чанков. */
    const auto root = temp_root();
    RagRegistry reg(root.string());

    auto a = reg.get("plugin_a");
    auto b = reg.get("plugin_b");
    TEST_ASSERT(a != nullptr && b != nullptr);
    TEST_ASSERT(a->scope() != b->scope());

    const RagStoreInfo ia = a->info();
    const RagStoreInfo ib = b->info();
    TEST_ASSERT(ia.path != ib.path);

    /* Оба каталога созданы внутри root. */
    TEST_ASSERT(ia.path.find(root.string()) == 0);
    TEST_ASSERT(ib.path.find(root.string()) == 0);
}

void test_registry_same_plugin_same_store() {
    /* Повторное обращение обязано вернуть ТОТ ЖЕ стор (тот же индекс),
 * иначе каждый вызов rag_search создавал бы новый пустой индекс. */
    const auto root = temp_root();
    RagRegistry reg(root.string());
    auto a1 = reg.get("wp_coder");
    auto a2 = reg.get("wp_coder");
    TEST_ASSERT(a1 == a2);
    TEST_ASSERT_EQUAL(reg.loaded_count(), 1u);
}

void test_registry_suffix_creates_distinct_store() {
    const auto root = temp_root();
    RagRegistry reg(root.string());
    auto a = reg.get("wp_coder", "");
    auto b = reg.get("wp_coder", "archive");
    TEST_ASSERT(a != b);
    TEST_ASSERT_EQUAL(a->scope(), "plugin:wp_coder");
    TEST_ASSERT_EQUAL(b->scope(), "plugin:wp_coder:archive");
}

void test_registry_lru_eviction() {
    const auto root = temp_root();
    RagRegistry reg(root.string(), nullptr, /*max_loaded=*/2);

    auto a = reg.get("a");
    auto b = reg.get("b");
    auto c = reg.get("c");   /* вытесняет самый старый (a) */
    TEST_ASSERT_EQUAL(reg.loaded_count(), 2u);

    /* a вытеснен: его деструктор уже отработал, но объект мог остаться
 * жить у нас в переменной (shared_ptr). Проверяем через реестр. */
    TEST_ASSERT(reg.info("a").reason.find("не загружен") != std::string::npos);
}

/* R4.5: вытесненный стор ФЛАШИТСЯ — его индекс переживает вытеснение.
 * 3 скоупа при max_loaded=2; самый старый (a) вытесняется и пишется на
 * диск. shared_ptr на a не держим: иначе деструктор не сработает. */
void test_registry_lru_eviction_flushes() {
    const auto root = temp_root();
    RagRegistry reg(root.string(), fallback_embeddings(), /*max_loaded=*/2);

    const std::string doc_a = (root / "a.txt").string();
    const std::string doc_b = (root / "b.txt").string();
    const std::string doc_c = (root / "c.txt").string();
    std::ofstream(doc_a) << "alpha lru marker\n";
    std::ofstream(doc_b) << "beta lru marker\n";
    std::ofstream(doc_c) << "gamma lru marker\n";

    /* Временные shared_ptr: после вызова ссылок на стор, кроме реестра, нет,
     * поэтому вытеснение реально уничтожает стор (и флашит). */
    TEST_ASSERT(reg.get("a")->index_document(doc_a) > 0);
    TEST_ASSERT(reg.get("b")->index_document(doc_b) > 0);
    TEST_ASSERT(reg.get("c")->index_document(doc_c) > 0);   /* вытесняет a */

    TEST_ASSERT_EQUAL(reg.loaded_count(), 2u);
    /* a вытеснен и сброшен на диск. */
    TEST_ASSERT(reg.info("a").reason.find("не загружен") != std::string::npos);

    /* Новый реестр над тем же корнем: a читается с диска (флаш сработал). */
    RagRegistry reg2(root.string(), fallback_embeddings(), /*max_loaded=*/4);
    auto a2 = reg2.get("a");
    /* search открывает стор; info() до этого вернул бы 0 (R1.2). */
    TEST_ASSERT(!a2->search("lru marker", 5, "").empty());
    TEST_ASSERT(a2->info().chunks > 0);
}

void test_registry_list_includes_disk_dirs() {
    const auto root = temp_root();
    RagRegistry reg(root.string());
    (void)reg.get("wp_coder");

    /* Каталог на диске есть → list() обязан его видеть даже если бы
 * реестр был новым (после перезапуска процесса). */
    RagRegistry fresh(root.string());
    const auto names = fresh.list();
    bool found = false;
    for (const auto& n : names) {
        if (n.find("wp_coder") != std::string::npos) found = true;
    }
    TEST_ASSERT_TRUE(found);
}

void test_registry_remove_deletes_dir() {
    const auto root = temp_root();
    RagRegistry reg(root.string());
    auto s = reg.get("wp_coder");
    const std::string path = s->info().path;
    TEST_ASSERT(std::filesystem::exists(path));

    TEST_ASSERT_TRUE(reg.remove("wp_coder"));
    TEST_ASSERT_FALSE(std::filesystem::exists(path));
}

/* R1.3, критерий приёмки: ОДИН генератор эмбеддингов на три стора.
 *
 * Модель эмбеддингов — 117 МБ, поэтому N скоупов не должны грузить N
 * копий. Проверять «load_model() вызван один раз» напрямую нечем (счётчика
 * нет, а заводить его в проде ради теста — пахнет тестовым крючком), но
 * шаринг наблюдаем: если стор создавал бы свой генератор, переданный
 * реестру экземпляр остался бы с use_count == 2 (тест + реестр). */
void test_registry_shares_one_embedding_generator() {
    const auto root = temp_root();

    /* Детерминированный эмбеддинг без модели и без сети: server_url
     * недостижим, load_model() в server-режиме возвращает true, а
     * generate_embedding() уходит в n-gram фолбэк. Это нужно, чтобы
     * info().embeddings был true и было что шарить. */
    auto gen = std::make_shared<EmbeddingGenerator>();
    gen->set_server_url("http://127.0.0.1:9");
    TEST_ASSERT_TRUE(gen->load_model());

    RagRegistry reg(root.string(), gen, /*max_loaded=*/4);
    auto a = reg.get("a");
    auto b = reg.get("b");
    auto c = reg.get("c");
    TEST_ASSERT(a && b && c);

    /* Реестр + три стора (+ их RagManager) держат ОДИН объект. */
    TEST_ASSERT(gen.use_count() >= 4);

    /* И каждый стор знает, что эмбеддинги есть: иначе поиск молча
     * возвращал бы пусто, а info() врал бы «нет эмбеддингов». */
    TEST_ASSERT_TRUE(a->info().embeddings);
    TEST_ASSERT_TRUE(b->info().embeddings);
    TEST_ASSERT_TRUE(c->info().embeddings);
}

/* R1.4: реестр и стор потокобезопасны.
 *
 * Плагин зовёт RAG из фонового потока (индексация тысяч страниц у
 * парсера сайтов — вообще асинхронная), а RagManager потокобезопасен
 * только на бумаге: у него есть progress_mutex_ и больше ничего.
 * Единственная защита — mtx_ стора и реестра, и порядок блокировок
 * registry → store (стор никогда не зовёт реестр, удерживая свой
 * мьютекс), поэтому цикла «A ждёт B, B ждёт A» быть не может.
 *
 * Тест дымовой: TSan в сборке нет, гонку на данных он не поймает. Он
 * ловит другое — реальную взаимоблокировку и порчу состояния при
 * одновременном создании сторов и флаше. Если порядок блокировок
 * нарушат (стор начнёт звать реестр), тест зависнет, а не «пройдёт». */
void test_registry_and_store_are_threadsafe() {
    const auto root = temp_root();
    auto gen = std::make_shared<EmbeddingGenerator>();
    gen->set_server_url("http://127.0.0.1:9");
    gen->load_model();

    RagRegistry reg(root.string(), gen, /*max_loaded=*/4);

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&, t] {
            const char* name = (t == 0) ? "wp_coder" : "siteparser";
            for (int i = 0; i < 40; ++i) {
                auto store = reg.get(name);
                if (!store) {
                    failures.fetch_add(1);
                    continue;
                }
                (void)store->search("запрос", 3);
                (void)store->info();
                if (t == 1 && i % 10 == 0) reg.flush_all();
            }
        });
    }
    for (auto& th : threads) th.join();

    TEST_ASSERT_EQUAL(failures.load(), 0);
    TEST_ASSERT_EQUAL(reg.loaded_count(), 2u);
}

int main() {
    REGISTER_TEST(test_scope_valid_plugin_name);
    REGISTER_TEST(test_scope_rejects_bad_plugin_names);
    REGISTER_TEST(test_scope_rejects_bad_suffix);
    REGISTER_TEST(test_scope_long_name_rejected);

    REGISTER_TEST(test_scope_dir_inside_root);
    REGISTER_TEST(test_scope_dir_rejects_empty);
    REGISTER_TEST(test_scope_dir_rejects_traversal);
    REGISTER_TEST(test_scope_dir_stable);

    REGISTER_TEST(test_options_empty_json_is_defaults);
    REGISTER_TEST(test_options_reads_fields);
    REGISTER_TEST(test_options_read_only_forbids_indexing);
    REGISTER_TEST(test_options_negative_clamped_to_zero);
    REGISTER_TEST(test_options_bad_json_is_error_but_not_crash);
    REGISTER_TEST(test_options_bad_field_type_ignored);

    REGISTER_TEST(test_options_reads_index_call_fields);
    REGISTER_TEST(test_options_index_fields_zero_means_scope_default);
    REGISTER_TEST(test_options_index_fields_clamped);
    REGISTER_TEST(test_options_index_bad_type_ignored);

    REGISTER_TEST(test_registry_get_creates_store);
    REGISTER_TEST(test_registry_rejects_bad_name);
    REGISTER_TEST(test_registry_two_plugins_are_isolated);
    REGISTER_TEST(test_registry_same_plugin_same_store);
    REGISTER_TEST(test_registry_suffix_creates_distinct_store);
    REGISTER_TEST(test_registry_lru_eviction);
    REGISTER_TEST(test_registry_lru_eviction_flushes);
    REGISTER_TEST(test_registry_list_includes_disk_dirs);
    REGISTER_TEST(test_registry_remove_deletes_dir);
    REGISTER_TEST(test_registry_shares_one_embedding_generator);
    REGISTER_TEST(test_registry_and_store_are_threadsafe);

    const int rc = test::TestRunner::instance().run();

    std::error_code ec;
    std::filesystem::remove_all(
        std::filesystem::temp_directory_path() /
        ("llama_gui_rag_registry_test_" + std::to_string((long long)::getpid())),
        ec);
    return rc;
}