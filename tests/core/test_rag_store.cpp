// test_rag_store.cpp — один изолированный индекс RAG (R1.1).
//
// Проверяется контракт стора:
//   - стор создаёт свой каталог и не трогает чужой;
//   - index_document без эмбеддингов возвращает 0 и не уносит чанки
//     в чужой индекс (индекс один — стор);
//   - search без эмбеддингов возвращает пусто (не «почти совпадение»);
//   - info() честно говорит embeddings=false / ready=false и называет
//     причину, а не притворяется готовым;
//   - flush() без менеджера → false, clear() не падает и не трогает
//     соседний стор;
//   - (USE_FAISS) индекс скоупа ложится В КАТАЛОГ СКОУПА, а не в
//     домашний каталог пользователя.
//
// Почему без эмбеддингов в большинстве тестов: модель 117 МБ и сервер в
// тесте недоступны, а контракт «нет эмбеддингов» — как раз то, что обязан
// выдерживать стор (R0.1). Там, где нужны настоящие чанки, используется
// детерминированный n-gram фолбэк (недостижимый server_url), а не модель.

#include "../../include/core/rag_store.h"
#include "../test_framework.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <unistd.h>
#include <fstream>
#include <string>

using namespace llama_gui::core;

namespace {

std::filesystem::path temp_root() {
    auto p = std::filesystem::temp_directory_path() /
             ("llama_gui_rag_store_test_" + std::to_string((long long)::getpid()));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

void write_file(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

} // namespace

void test_store_creates_its_directory() {
    const auto root = temp_root();
    const std::string path = (root / "plugin_a").string();

    RagStore store("plugin:a", path, nullptr);
    TEST_ASSERT_TRUE(std::filesystem::exists(path));
}

void test_store_info_unavailable_is_honest() {
    const auto root = temp_root();
    RagStore store("plugin:a", (root / "a").string(), nullptr);

    const RagStoreInfo info = store.info();
    TEST_ASSERT_EQUAL(info.scope, "plugin:a");
    TEST_ASSERT_FALSE(info.embeddings);
    TEST_ASSERT_FALSE(info.ready);
    /* Причина обязана быть названа: «не готово» без причины заставляет
     * вызывающего гадать, а гадать он будет неверно. */
    TEST_ASSERT_FALSE(info.reason.empty());
}

void test_store_index_document_without_embeddings_returns_zero() {
    const auto root = temp_root();
    const std::string path = (root / "a").string();
    RagStore store("plugin:a", path, nullptr);

    const std::string doc = (root / "doc.txt").string();
    write_file(doc, "hello world, this is a test document");

    /* Без эмбеддингов индексация невозможна: чанк без вектора бесполезен
     * для поиска. Ноль — честный ответ, а не « куча чанков». */
    TEST_ASSERT_EQUAL(store.index_document(doc), 0);
}

void test_store_search_without_embeddings_returns_empty() {
    const auto root = temp_root();
    RagStore store("plugin:a", (root / "a").string(), nullptr);

    const auto results = store.search("anything", 5);
    TEST_ASSERT_EQUAL(results.size(), 0u);
}

void test_store_flush_without_manager_returns_false() {
    const auto root = temp_root();
    RagStore store("plugin:a", (root / "a").string(), nullptr);
    // Менеджера ещё нет (ни один метод его не создал) — флашить нечего.
    TEST_ASSERT_FALSE(store.flush());
}

void test_store_clear_does_not_touch_neighbour() {
    const auto root = temp_root();
    const std::string a = (root / "a").string();
    const std::string b = (root / "b").string();

    RagStore sa("plugin:a", a, nullptr);
    RagStore sb("plugin:b", b, nullptr);

    /* Каталоги созданы; в b положим файл-маркер, чтобы проверить, что
     * clear() стора A не уносит каталог стора B. */
    write_file((std::filesystem::path(b) / "marker.txt").string(), "keep me");

    sa.clear();

    TEST_ASSERT_TRUE(std::filesystem::exists(a));
    TEST_ASSERT_TRUE(std::filesystem::exists(b));
    TEST_ASSERT_TRUE(std::filesystem::exists(std::filesystem::path(b) / "marker.txt"));
}

void test_store_scope_is_preserved() {
    const auto root = temp_root();
    RagStore store("plugin:wp_coder:archive", (root / "x").string(), nullptr);
    TEST_ASSERT_EQUAL(store.scope(), "plugin:wp_coder:archive");
}

#ifdef USE_FAISS
/* R1.1, главное свойство: индекс скоупа лежит В КАТАЛОГЕ СКОУПА.
 *
 * До правки RagManager создавался без профиля, и save_index() при пустом
 * профиле молча уходил в `~/.llama-gui/rag_indexes/`. То есть все скоупы
 * писали в ОДИН файл пользователя: изоляция существовала только в памяти
 * процесса и исчезала при первом же flush().
 *
 * Проверяем обе стороны: файлы появились у стора И в домашнем каталоге не
 * появилось ничего. Второе важнее: «записал куда-то» — не то же самое,
 * что «записал, куда обещал». */
void test_store_writes_index_into_its_own_directory() {
    const auto root = temp_root();

    /* Подменяем HOME: get_default_index_path() берёт путь оттуда. Без
     * подмены тест либо писал бы в реальный индекс пользователя, либо
     * «проходил» бы по уже существующему чужому индексу. */
    const std::string fake_home = (root / "home").string();
    std::filesystem::create_directories(fake_home);
    setenv("HOME", fake_home.c_str(), 1);

    /* Эмбеддинги без модели и без сети: server_url недостижим, load_model()
     * в server-режиме возвращает true, а generate_embedding() уходит в
     * n-gram фолбэк — детерминированный вектор. */
    auto gen = std::make_shared<EmbeddingGenerator>();
    gen->set_server_url("http://127.0.0.1:9");
    TEST_ASSERT_TRUE(gen->load_model());

    const std::string path = (root / "plugin_wp_coder").string();
    RagStore store("plugin:wp_coder", path, gen);

    const std::string doc = (root / "doc.txt").string();
    write_file(doc,
               "add_action('init', 'my_plugin_init');\n"
               "function my_plugin_init() { /* hook registration */ }\n");
    TEST_ASSERT(store.index_document(doc) > 0);
    TEST_ASSERT_TRUE(store.flush());

    const std::string index_file =
        (std::filesystem::path(path) / "default_index.faiss").string();
    TEST_ASSERT_TRUE(std::filesystem::exists(index_file));
    TEST_ASSERT_TRUE(std::filesystem::exists(index_file + ".metadata.json"));

    /* ... и ничего не появилось в домашнем каталоге пользователя.
     * get_default_index_path() создаёт каталог через mkdir -p, поэтому
     * проверяем не наличие каталога, а отсутствие файлов в нём. */
    const auto default_dir =
        std::filesystem::path(fake_home) / ".llama-gui" / "rag_indexes";
    size_t stray = 0;
    if (std::filesystem::exists(default_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(default_dir)) {
            if (entry.is_regular_file()) stray++;
        }
    }
    TEST_ASSERT_EQUAL(stray, 0u);
}
#endif

int main() {
    REGISTER_TEST(test_store_creates_its_directory);
    REGISTER_TEST(test_store_info_unavailable_is_honest);
    REGISTER_TEST(test_store_index_document_without_embeddings_returns_zero);
    REGISTER_TEST(test_store_search_without_embeddings_returns_empty);
    REGISTER_TEST(test_store_flush_without_manager_returns_false);
    REGISTER_TEST(test_store_clear_does_not_touch_neighbour);
    REGISTER_TEST(test_store_scope_is_preserved);
#ifdef USE_FAISS
    REGISTER_TEST(test_store_writes_index_into_its_own_directory);
#endif

    const int rc = test::TestRunner::instance().run();

    std::error_code ec;
    std::filesystem::remove_all(
        std::filesystem::temp_directory_path() /
        ("llama_gui_rag_store_test_" + std::to_string((long long)::getpid())),
        ec);
    return rc;
}