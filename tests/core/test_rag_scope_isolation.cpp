// test_rag_scope_isolation.cpp — изоляция скоупов RAG (R1.6).
//
//   - документ скоупа A не находится в скоупе B (и наоборот);
//   - clear(A) не трогает B;
//   - повторная индексация того же документа не дублирует чанки.
//
// ПОЧЕМУ ЭТО НЕ ПУСТОЙ ТЕСТ. Раньше изоляция проверялась бы на двух
// ПУСТЫХ индексах: без эмбеддингов чанки не создаются вообще
// (`rag_manager_documents.cpp` пропускает чанк с пустым вектором), и
// «B не нашёл» было бы верно ровно потому, что в B ничего нет. Здесь
// эмбеддинги есть, поэтому каждый тест сначала убеждается, что индекс
// непустой (`info().chunks > 0`), и только потом проверяет, что чужое
// не видно.
//
// ГДЕ БЕРУТСЯ ЭМБЕДДИНГИ. Модель (117 МБ) в тесте недоступна, а сервер
// недостижим. Используется штатный путь отказа: `set_server_url()` на
// заведомо мёртвый адрес → `load_model()` в server-режиме возвращает true,
// `generate_embedding()` не получает ответа и уходит в детерминированный
// n-gram фолбэк. Векторы детерминированы, а значит тест воспроизводим;
// качество «похожести» здесь не проверяется — проверяется, что чанк
// физически лежит в индексе другого скоупа или нет.
//
// Только под USE_FAISS: без FAISS поиск идёт брутфорсом по тем же чанкам,
// и отдельно проверять эту ветку здесь нечего.

#include "../../include/core/rag_registry.h"
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
             ("llama_gui_rag_scope_test_" + std::to_string((long long)::getpid()));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

void write_file(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

/* Детерминированный генератор эмбеддингов без модели и без сети.
 * См. «ГДЕ БЕРУТСЯ ЭМБЕДДИНГИ» в шапке файла. */
std::shared_ptr<EmbeddingGenerator> fallback_embeddings() {
    auto gen = std::make_shared<EmbeddingGenerator>();
    gen->set_server_url("http://127.0.0.1:9");   /* заведомо мёртвый адрес */
    if (!gen->load_model()) {
        std::cerr << "test: cannot prepare fallback embeddings" << std::endl;
        std::abort();
    }
    return gen;
}

} // namespace

#ifdef USE_FAISS

void test_scope_isolation_search() {
    const auto root = temp_root();
    RagRegistry reg(root.string(), fallback_embeddings());

    const std::string doc_a = (root / "doc_a.txt").string();
    const std::string doc_b = (root / "doc_b.txt").string();
    const std::string text_a = "alpha unique marker for scope A";
    const std::string text_b = "beta unique marker for scope B";
    write_file(doc_a, text_a + "\n");
    write_file(doc_b, text_b + "\n");

    auto a = reg.get("plugin_a");
    auto b = reg.get("plugin_b");
    TEST_ASSERT(a && b);
    TEST_ASSERT(a->index_document(doc_a) > 0);
    TEST_ASSERT(b->index_document(doc_b) > 0);

    /* Индексы непустые: иначе «не нашёл» ничего не доказывает. */
    TEST_ASSERT(a->info().chunks > 0);
    TEST_ASSERT(b->info().chunks > 0);

    /* A находит СВОЙ документ, и только его. */
    const auto in_a = a->search(text_a, 5);
    TEST_ASSERT(!in_a.empty());
    for (const auto& chunk : in_a) {
        TEST_ASSERT_EQUAL(chunk.file_path, doc_a);
    }

    /* Главное свойство: выдача A не содержит чанков B.
     *
     * Проверяем именно это, а не «поиск по тексту B вернул пусто»: у
     * n-gram фолбэка «alpha unique marker for scope A» и «beta unique
     * marker for scope B» много общих n-грамм, и сходство легко
     * переваливает порог 0.7 — тогда A вернёт СВОЙ чанк, и это не утечка.
     * Утечка — это чужой file_path в выдаче. */
    for (const auto& chunk : a->search(text_b, 5)) {
        TEST_ASSERT_EQUAL(chunk.file_path, doc_a);
    }
    /* Число чанков — второй, независимый аргумент: чужой документ не
     * попал в индекс A (иначе было бы 2). */
    TEST_ASSERT_EQUAL(a->info().chunks, 1u);
    TEST_ASSERT_EQUAL(a->info().documents, 1u);

    /* Симметрично: B не видит A. */
    const auto in_b = b->search(text_b, 5);
    TEST_ASSERT(!in_b.empty());
    for (const auto& chunk : in_b) {
        TEST_ASSERT_EQUAL(chunk.file_path, doc_b);
    }
    for (const auto& chunk : b->search(text_a, 5)) {
        TEST_ASSERT_EQUAL(chunk.file_path, doc_b);
    }
    TEST_ASSERT_EQUAL(b->info().chunks, 1u);
    TEST_ASSERT_EQUAL(b->info().documents, 1u);
}

void test_scope_clear_does_not_touch_neighbour() {
    const auto root = temp_root();
    RagRegistry reg(root.string(), fallback_embeddings());

    const std::string doc_a = (root / "doc_a.txt").string();
    const std::string doc_b = (root / "doc_b.txt").string();
    const std::string text_a = "alpha unique marker for scope A";
    const std::string text_b = "beta unique marker for scope B";
    write_file(doc_a, text_a + "\n");
    write_file(doc_b, text_b + "\n");

    auto a = reg.get("plugin_a");
    auto b = reg.get("plugin_b");
    TEST_ASSERT(a && b);
    TEST_ASSERT(a->index_document(doc_a) > 0);
    TEST_ASSERT(b->index_document(doc_b) > 0);

    a->clear();

    TEST_ASSERT_EQUAL(a->info().chunks, 0u);
    TEST_ASSERT_EQUAL(a->search(text_a, 5).size(), 0u);

    /* Сосед не пострадал: ни чанки, ни возможность искать. */
    TEST_ASSERT(b->info().chunks > 0);
    TEST_ASSERT(!b->search(text_b, 5).empty());
}

void test_scope_reindex_does_not_duplicate_chunks() {
    const auto root = temp_root();
    RagRegistry reg(root.string(), fallback_embeddings());

    const std::string doc_a = (root / "doc_a.txt").string();
    const std::string text_a = "alpha unique marker for scope A";
    write_file(doc_a, text_a + "\n");

    auto a = reg.get("plugin_a");
    TEST_ASSERT(a);
    TEST_ASSERT(a->index_document(doc_a) > 0);

    const size_t chunks_after_first = a->info().chunks;
    const size_t docs_after_first = a->info().documents;
    TEST_ASSERT(chunks_after_first > 0);

    /* Второй вызов с тем же путём: ничего нового, ноль чанков. */
    TEST_ASSERT_EQUAL(a->index_document(doc_a), 0);
    TEST_ASSERT_EQUAL(a->info().chunks, chunks_after_first);
    TEST_ASSERT_EQUAL(a->info().documents, docs_after_first);

    /* И в выдаче не появилось второго экземпляра того же чанка. */
    TEST_ASSERT_EQUAL(a->search(text_a, 10).size(), 1u);
}

#endif // USE_FAISS

int main() {
#ifdef USE_FAISS
    REGISTER_TEST(test_scope_isolation_search);
    REGISTER_TEST(test_scope_clear_does_not_touch_neighbour);
    REGISTER_TEST(test_scope_reindex_does_not_duplicate_chunks);
#else
    std::cerr << "test_rag_scope_isolation: USE_FAISS is off — nothing to test"
              << std::endl;
#endif

    const int rc = test::TestRunner::instance().run();

    std::error_code ec;
    std::filesystem::remove_all(
        std::filesystem::temp_directory_path() /
        ("llama_gui_rag_scope_test_" + std::to_string((long long)::getpid())),
        ec);
    return rc;
}