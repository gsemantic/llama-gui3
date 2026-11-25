// test_rag_quota.cpp — квоты индексации (R4.2).
//
//   - в пределах квот: чанки добавлены, truncated=false;
//   - max_bytes (на документ): документ слишком велик → truncated, 0 чанков;
//   - max_chunks (на документ): чанков больше лимита → откат, truncated;
//   - max_index_bytes (на индекс): индекс перерос лимит → откат, truncated;
//   - read-only скоуп: не индексирует, но это НЕ truncated (режим, не квота);
//   - reindex: изменённый документ переиндексируется только с флагом;
//   - после отката индекс пуст (документ не остался «проиндексированным»).
//
// Эмбеддинги — детерминированный n-gram фолбэк (недостижимый server_url),
// как в test_rag_scope_isolation: без модели и без сети.

#include "../../include/core/rag_store.h"
#include "../test_framework.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>

using namespace llama_gui::core;

namespace {

std::filesystem::path temp_root() {
    auto p = std::filesystem::temp_directory_path() /
             ("llama_gui_rag_quota_test_" + std::to_string((long long)::getpid()));
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

/* Текст, который режется на много чанков: предложения с точками —
 * split_into_chunks режет по границам предложений, а не по строкам. */
std::string many_lines(int n) {
    std::string s;
    for (int i = 0; i < n; ++i) {
        s += "Line number " + std::to_string(i) +
             " has several words to form a sentence of reasonable size. ";
    }
    return s;
}

} // namespace

void test_within_quota_indexes() {
    const auto root = temp_root();
    RagStore store("plugin:a", (root / "a").string(), fallback_embeddings());
    const std::string doc = (root / "doc.txt").string();
    write_file(doc, "alpha unique marker content\n");

    RagScopeOptions opts;   /* все нули = без квот */
    const auto r = store.index_document(doc, opts);
    TEST_ASSERT(!r.truncated);
    TEST_ASSERT(r.chunks > 0);
    TEST_ASSERT(r.reason.empty());
}

void test_max_bytes_truncates() {
    const auto root = temp_root();
    RagStore store("plugin:a", (root / "a").string(), fallback_embeddings());
    const std::string doc = (root / "big.txt").string();
    write_file(doc, many_lines(50));   /* заведомо больше 100 байт */

    RagScopeOptions opts;
    opts.max_bytes = 100;
    const auto r = store.index_document(doc, opts);
    TEST_ASSERT(r.truncated);
    TEST_ASSERT_EQUAL(r.chunks, 0);
    TEST_ASSERT(r.reason.find("max_bytes") != std::string::npos);
    /* Ничего не осталось в индексе. */
    TEST_ASSERT_EQUAL(store.info().chunks, 0u);
}

void test_max_chunks_truncates_and_rolls_back() {
    const auto root = temp_root();
    RagStore store("plugin:a", (root / "a").string(), fallback_embeddings());
    const std::string doc = (root / "doc.txt").string();
    write_file(doc, many_lines(500));

    RagScopeOptions opts;
    opts.max_chunks = 1;
    const auto r = store.index_document(doc, opts);
    TEST_ASSERT(r.truncated);
    TEST_ASSERT_EQUAL(r.chunks, 0);   /* откат: документ не добавлен */
    TEST_ASSERT(r.reason.find("max_chunks") != std::string::npos);
    TEST_ASSERT_EQUAL(store.info().chunks, 0u);
}

void test_max_index_bytes_truncates() {
    const auto root = temp_root();
    RagStore store("plugin:a", (root / "a").string(), fallback_embeddings());
    const std::string doc = (root / "doc.txt").string();
    write_file(doc, "alpha unique marker content\n");

    RagScopeOptions opts;
    opts.max_index_bytes = 1;   /* любой непустой индекс больше */
    const auto r = store.index_document(doc, opts);
    TEST_ASSERT(r.truncated);
    TEST_ASSERT_EQUAL(r.chunks, 0);
    TEST_ASSERT(r.reason.find("max_index_bytes") != std::string::npos);
}

void test_read_only_scope_does_not_index() {
    const auto root = temp_root();
    RagStore store("plugin:a", (root / "a").string(), fallback_embeddings());
    const std::string doc = (root / "doc.txt").string();
    write_file(doc, "alpha unique marker\n");

    RagScopeOptions opts;
    opts.allow_index = false;   /* read-only скоуп */
    const auto r = store.index_document(doc, opts);
    TEST_ASSERT_EQUAL(r.chunks, 0);
    /* Это режим, а не квота: truncated остаётся false. */
    TEST_ASSERT(!r.truncated);
    TEST_ASSERT(r.reason.find("read-only") != std::string::npos);
}

void test_reindex_updates_changed_document() {
    const auto root = temp_root();
    RagStore store("plugin:a", (root / "a").string(), fallback_embeddings());
    const std::string doc = (root / "doc.txt").string();
    write_file(doc, "first version marker\n");

    RagScopeOptions plain;
    TEST_ASSERT(store.index_document(doc, plain).chunks > 0);
    const size_t after_first = store.info().chunks;

    /* Файл изменился. Без reindex повторный вызов — «уже индексирован». */
    write_file(doc, "second version marker with different words\n");
    const auto again = store.index_document(doc, plain);
    TEST_ASSERT_EQUAL(again.chunks, 0);
    TEST_ASSERT(!again.truncated);
    TEST_ASSERT_EQUAL(store.info().chunks, after_first);

    /* С reindex документ переиндексируется (старые чанки убраны). */
    RagScopeOptions reindex;
    reindex.reindex = true;
    const auto re = store.index_document(doc, reindex);
    TEST_ASSERT(re.chunks > 0);
    TEST_ASSERT(!re.truncated);
}

int main() {
    REGISTER_TEST(test_within_quota_indexes);
    REGISTER_TEST(test_max_bytes_truncates);
    REGISTER_TEST(test_max_chunks_truncates_and_rolls_back);
    REGISTER_TEST(test_max_index_bytes_truncates);
    REGISTER_TEST(test_read_only_scope_does_not_index);
    REGISTER_TEST(test_reindex_updates_changed_document);

    const int rc = test::TestRunner::instance().run();

    std::error_code ec;
    std::filesystem::remove_all(
        std::filesystem::temp_directory_path() /
        ("llama_gui_rag_quota_test_" + std::to_string((long long)::getpid())),
        ec);
    return rc;
}