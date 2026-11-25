#pragma once

/*
 * plugin_rag_index.h — асинхронная индексация с отменой (R4.3).
 *
 * ЗАЧЕМ. `rag_index_document_ex` синхронный: парсер сайтов, которому нужно
 * проиндексировать тысячи страниц, не может прервать одну «на лету» и
 * обязан ждать. Асинхронная пара `rag_index_start`/`rag_index_wait` +
 * `rag_index_cancel` даёт хендл: индексация идёт в фоне, отмена ставит
 * атомарный флаг (проверяется в цикле чанков RagManager), wait забирает
 * результат.
 *
 * ЧТО ЗДЕСЬ ЕСТЬ. Чистая логика без `LlamaPluginHost*`: job создаётся,
 * живёт в фоне, отменяется и забирается. Хост (`plugin_manager.cpp`) лишь
 * собирает `RagCallContext` и зовёт эти функции — та же причина, что у
 * plugin_rag_api.cpp.
 *
 * ЖИЗНЕННЫЙ ЦИКЛ. start → (wait)*, cancel? → wait(завершение). Память job
 * освобождается в wait, когда индексация завершена. wait с таймаутом, не
 * дождавшись, возвращает {"code":"timeout"} и job ЖИВ — зовите wait ещё.
 * Поэтому cancel вызывают ДО финального wait (пока job жив).
 */

#include "plugins/plugin_rag_api.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace llama_gui {
namespace plugin {

struct RagIndexJob {
    std::atomic<bool> cancel{false};
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    bool freed = false;
    core::RagStore::IndexResult result;
    std::string error;
    std::thread worker;
};

/* Запуск индексации в фоне. Возвращает job или nullptr (причина в error).
 * Проверки — те же, что у `_ex`: disabled → unavailable → denied → bad_scope
 * → bad_request. */
RagIndexJob* rag_index_start(const RagCallContext& ctx,
                             const std::string& requested_scope,
                             const std::string& path,
                             const std::string& opts_json,
                             std::string* error);

/* Дождаться завершения (timeout_ms <= 0 — бесконечно). Возвращает malloc-
 * строку JSON (освобождает вызывающий, в ABI — free_string). Когда job
 * завершён — освобождает его; при таймауте job остаётся жив. */
char* rag_index_wait(RagIndexJob* job, int timeout_ms);

/* Попросить отмену (потокобезопасно; job должен быть жив). */
void rag_index_cancel(RagIndexJob* job);

} // namespace plugin
} // namespace llama_gui