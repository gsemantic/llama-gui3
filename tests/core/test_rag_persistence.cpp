// test_rag_persistence.cpp — индекс переживает выгрузку/перезапуск (R4.4).
//
//   - создали → проиндексировали → flush → уничтожили реестр (выгрузка);
//   - новый реестр над тем же корнем → тот же скоуп → документ НАХОДИТСЯ;
//   - чужой скоуп по-прежнему не видит документ (изоляция на диске).
//
// Эмбеддинги — детерминированный n-gram фолбэк: векторы воспроизводимы,
// поэтому поиск после перезапуска находит тот же чанк без модели и сети.
//
// ВАЖНО: `info()` НЕ открывает стор (R1.2) — до первого search/index
// менеджер не создан и чанков «в памяти» нет. Поэтому после перезапуска
// сначала идёт search (он открывает индекс с диска), потом info.

#include "../../include/core/rag_registry.h"
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
             ("llama_gui_rag_persist_test_" + std::to_string((long long)::getpid()));
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
    gen->set_server_url("http://127.0.0.1:9");
    if (!gen->load_model()) {
        std::cerr << "test: cannot prepare fallback embeddings" << std::endl;
        std::abort();
    }
    return gen;
}

} // namespace

void test_index_survives_restart() {
    const auto root = temp_root();
    const std::string doc = (root / "doc.txt").string();
    const std::string text = "persistence unique marker for wp_coder scope";
    write_file(doc, text + "\n");

    {
        RagRegistry reg(root.string(), fallback_embeddings());
        auto store = reg.get("wp_coder");
        TEST_ASSERT(store != nullptr);
        TEST_ASSERT(store->index_document(doc) > 0);
        /* Без явного flush: индекс должен пережить выгрузку за счёт
         * флаша в деструкторе стора/реестра (R4.4). */
    }
    /* Реестр уничтожен — как при выгрузке плагина/перезапуске. */

    {
        RagRegistry reg(root.string(), fallback_embeddings());
        auto store = reg.get("wp_coder");
        TEST_ASSERT(store != nullptr);

        /* search открывает индекс с диска (info() этого не делает). */
        const auto chunks = store->search(text, 5, "");
        TEST_ASSERT(!chunks.empty());
        bool found = false;
        for (const auto& c : chunks) {
            if (c.file_path == doc) found = true;
        }
        TEST_ASSERT(found);
        /* После открытия — чанки в памяти. */
        TEST_ASSERT(store->info().chunks > 0);
    }
}

void test_isolation_survives_restart() {
    const auto root = temp_root();
    const std::string doc_a = (root / "a.txt").string();
    write_file(doc_a, "alpha persistence marker\n");

    {
        RagRegistry reg(root.string(), fallback_embeddings());
        TEST_ASSERT(reg.get("plugin_a")->index_document(doc_a) > 0);
        reg.flush_all();
    }

    {
        RagRegistry reg(root.string(), fallback_embeddings());
        /* plugin_b не видит документ plugin_a: у него свой каталог. */
        const auto chunks = reg.get("plugin_b")->search("persistence marker", 5, "");
        for (const auto& c : chunks) {
            TEST_ASSERT(c.file_path != doc_a);
        }
        /* А plugin_a — видит (открываем индекс и проверяем). */
        reg.get("plugin_a")->search("persistence marker", 5, "");
        TEST_ASSERT(reg.get("plugin_a")->info().chunks > 0);
    }
}

int main() {
    REGISTER_TEST(test_index_survives_restart);
    REGISTER_TEST(test_isolation_survives_restart);

    const int rc = test::TestRunner::instance().run();

    std::error_code ec;
    std::filesystem::remove_all(
        std::filesystem::temp_directory_path() /
        ("llama_gui_rag_persist_test_" + std::to_string((long long)::getpid())),
        ec);
    return rc;
}