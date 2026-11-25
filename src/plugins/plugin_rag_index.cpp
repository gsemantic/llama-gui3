/*
 * plugin_rag_index.cpp — асинхронная индексация с отменой (R4.3).
 *
 * Обоснование и границы — в plugin_rag_index.h. Здесь только код.
 */

#include "plugins/plugin_rag_index.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <utility>

namespace llama_gui {
namespace plugin {

namespace {

using json = nlohmann::json;
using clock = std::chrono::steady_clock;

char* dup_string(const std::string& s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

/* Общая проверка вызова (как begin() в plugin_rag_api.cpp, но без скоупа-
 * суффикса наружу: он нужен только чтобы открыть стор). */
bool validate(const RagCallContext& ctx, const std::string& requested_scope,
              const std::string& path, std::string& suffix,
              std::string& error) {
    if (!ctx.allowed) { error = "RAG is disabled in settings"; return false; }
    if (!ctx.plugin_enabled) { error = "RAG is disabled for this plugin"; return false; }
    if (!ctx.registry) { error = "RAG registry is not configured"; return false; }
    if (!ctx.has_rag_write) { error = "plugin lacks permission 'rag_write'"; return false; }
    if (path.empty()) { error = "empty path"; return false; }
    return resolve_scope(ctx.plugin_name, requested_scope, suffix, error);
}

} // namespace

RagIndexJob* rag_index_start(const RagCallContext& ctx,
                             const std::string& requested_scope,
                             const std::string& path,
                             const std::string& opts_json,
                             std::string* error) {
    auto fail = [&](const std::string& why) -> RagIndexJob* {
        if (error) *error = why;
        return nullptr;
    };

    std::string suffix;
    std::string why;
    if (!validate(ctx, requested_scope, path, suffix, why)) return fail(why);

    core::RagScopeOptions opts;
    std::string opts_error;
    if (!core::parse_scope_options(opts_json, opts, &opts_error)) {
        return fail("invalid opts_json: " + opts_error);
    }

    std::shared_ptr<core::RagStore> store;
    try {
        store = ctx.registry->get(ctx.plugin_name, suffix, &why);
    } catch (const std::exception& e) {
        return fail(std::string("cannot open scope: ") + e.what());
    }
    if (!store) return fail("cannot open scope: " + why);

    auto* job = new RagIndexJob();
    job->worker = std::thread([job, store, path, opts]() {
        job->result = store->index_document(path, opts, &job->cancel);
        {
            std::lock_guard<std::mutex> lk(job->mtx);
            job->done = true;
        }
        job->cv.notify_all();
    });
    return job;
}

char* rag_index_wait(RagIndexJob* job, int timeout_ms) {
    if (!job) return dup_string(R"({"ok":false,"code":"bad_request","error":"no job"})");

    bool done;
    {
        std::unique_lock<std::mutex> lk(job->mtx);
        if (timeout_ms <= 0) {
            job->cv.wait(lk, [job] { return job->done; });
        } else {
            job->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                             [job] { return job->done; });
        }
        done = job->done;
    }

    if (!done) {
        /* Таймаут: job жив, зовите wait ещё. */
        return dup_string(R"({"ok":false,"code":"timeout","error":"still indexing"})");
    }

    json j;
    j["ok"] = true;
    j["chunks"] = job->result.chunks;
    j["truncated"] = job->result.truncated;
    j["reason"] = job->result.reason;
    const std::string out = j.dump();

    /* Завершён — освобождаем job (worker уже приостановлен). */
    if (job->worker.joinable()) job->worker.join();
    delete job;
    return dup_string(out);
}

void rag_index_cancel(RagIndexJob* job) {
    if (!job) return;
    /* Ставим флаг; worker увидит его в цикле чанков и выйдет. */
    job->cancel.store(true);
}

} // namespace plugin
} // namespace llama_gui