// test_rag_cancel.cpp — асинхронная индексация и отмена (R4.3).
//
//   - start + wait: индексация в фоне завершается, chunks > 0;
//   - deadline_ms=1 на большом документе: цикл чанков прерывается →
//     truncated, reason "deadline exceeded", и БЫСТРО (< 3 с);
//   - отмена флагом (store-level, детерминированно): pre-set cancel →
//     truncated "cancelled", 0 чанков;
//   - start без rag_write → nullptr (отказ);
//   - async cancel на большом документе → truncated (отмена «на лету»).
//
// Эмбеддинги — детерминированный n-gram фолбэк (недостижимый server_url).

#include "plugins/plugin_rag_index.h"
#include "plugins/plugin_rag_api.h"

#include "../test_framework.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>

using namespace llama_gui::core;
using namespace llama_gui::plugin;
using json = nlohmann::json;

namespace {

std::filesystem::path temp_root() {
    auto p = std::filesystem::temp_directory_path() /
             ("llama_gui_rag_cancel_test_" + std::to_string((long long)::getpid()));
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

std::string sentences(int n) {
    std::string s;
    for (int i = 0; i < n; ++i) {
        s += "Sentence number " + std::to_string(i) +
             " with several words to be split into a chunk.\n";
    }
    return s;
}

json take_json(char* p) {
    if (!p) return json();
    json j = json::parse(p, nullptr, /*allow_exceptions=*/false);
    std::free(p);
    return j;
}

RagCallContext ctx_for(RagRegistry& reg, bool write = true) {
    RagCallContext ctx;
    ctx.registry = &reg;
    ctx.allowed = true;
    ctx.plugin_name = "wp_coder";
    ctx.has_rag_access = true;
    ctx.has_rag_write = write;
    ctx.plugin_enabled = true;
    return ctx;
}

} // namespace

void test_async_index_completes() {
    const auto root = temp_root();
    RagRegistry reg(root.string(), fallback_embeddings());
    const std::string doc = (root / "doc.txt").string();
    write_file(doc, sentences(20));

    std::string error;
    RagIndexJob* job = rag_index_start(ctx_for(reg), "", doc, "", &error);
    TEST_ASSERT(job != nullptr);

    const json j = take_json(rag_index_wait(job, /*timeout_ms=*/0));
    TEST_ASSERT_EQUAL(j.value("ok", false), true);
    TEST_ASSERT(j.value("chunks", 0) > 0);
    TEST_ASSERT_EQUAL(j.value("truncated", true), false);
}

void test_deadline_interrupts_quickly() {
    const auto root = temp_root();
    RagRegistry reg(root.string(), fallback_embeddings());
    const std::string doc = (root / "big.txt").string();
    write_file(doc, sentences(20000));   /* заведомо дольше 1 мс */

    RagIndexJob* job = rag_index_start(ctx_for(reg), "", doc,
                                       R"({"deadline_ms":1})", nullptr);
    TEST_ASSERT(job != nullptr);

    const auto t0 = std::chrono::steady_clock::now();
    const json j = take_json(rag_index_wait(job, /*timeout_ms=*/0));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();

    TEST_ASSERT_EQUAL(j.value("ok", false), true);
    TEST_ASSERT_EQUAL(j.value("truncated", false), true);
    /* Прерывание «на лету», а не после полной обработки. */
    TEST_ASSERT(ms < 3000);
}

void test_cancel_flag_prevents_indexing() {
    /* Детерминированно: флаг отмены уже выставлен до старта. */
    const auto root = temp_root();
    RagStore store("plugin:wp_coder", (root / "s").string(),
                   fallback_embeddings());
    const std::string doc = (root / "doc.txt").string();
    write_file(doc, sentences(50));

    std::atomic<bool> cancel{true};
    RagScopeOptions opts;
    const auto r = store.index_document(doc, opts, &cancel);
    TEST_ASSERT(r.truncated);
    TEST_ASSERT_EQUAL(r.chunks, 0);
    TEST_ASSERT(r.reason.find("cancel") != std::string::npos);
}

void test_start_requires_write_permission() {
    const auto root = temp_root();
    RagRegistry reg(root.string(), fallback_embeddings());
    const std::string doc = (root / "doc.txt").string();
    write_file(doc, "alpha\n");

    std::string error;
    RagIndexJob* job = rag_index_start(ctx_for(reg, /*write=*/false), "", doc,
                                       "", &error);
    TEST_ASSERT(job == nullptr);
    TEST_ASSERT(!error.empty());
}

void test_async_cancel_interrupts() {
    const auto root = temp_root();
    RagRegistry reg(root.string(), fallback_embeddings());
    const std::string doc = (root / "big.txt").string();
    write_file(doc, sentences(20000));

    RagIndexJob* job = rag_index_start(ctx_for(reg), "", doc, "", nullptr);
    TEST_ASSERT(job != nullptr);

    /* Отмена сразу после старта — worker увидит флаг в цикле чанков. */
    rag_index_cancel(job);
    const json j = take_json(rag_index_wait(job, /*timeout_ms=*/0));
    TEST_ASSERT_EQUAL(j.value("ok", false), true);
    TEST_ASSERT_EQUAL(j.value("truncated", false), true);
}

int main() {
    REGISTER_TEST(test_async_index_completes);
    REGISTER_TEST(test_deadline_interrupts_quickly);
    REGISTER_TEST(test_cancel_flag_prevents_indexing);
    REGISTER_TEST(test_start_requires_write_permission);
    REGISTER_TEST(test_async_cancel_interrupts);

    const int rc = test::TestRunner::instance().run();

    std::error_code ec;
    std::filesystem::remove_all(
        std::filesystem::temp_directory_path() /
        ("llama_gui_rag_cancel_test_" + std::to_string((long long)::getpid())),
        ec);
    return rc;
}