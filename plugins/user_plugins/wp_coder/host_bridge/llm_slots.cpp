// llm_slots.cpp — слоты LLM и AST по возможностям хоста (И13.8).
//
// Тела перенесены из src/plugin_main.cpp без изменения поведения, кроме
// одного места, названного в install_llm_chat ниже.

#include "llm_slots.h"

#include "llm_blocking.h"
#include "llm_stream_shim.h"
#include "ast_symbols.h"

#include "core/json_utils.h"
#include "core/llm_error.h"
#include "core/retry.h"

#include <chrono>
#include <thread>

namespace coder {
namespace host {

namespace {

/* Разбор JSON от llm_chat_messages (ok/content/usage).
 * Общий парсер — core/json_utils.h. */
void parse_chat_reply(const std::string& body, LlmReply& out) {
    out.content = json::str(body, "content");
    out.finish_reason = json::str(body, "finish_reason");
    if (out.finish_reason.empty()) out.finish_reason = "stop";
    out.prompt_tokens = json::int_(body, "prompt_tokens");
    out.completion_tokens = json::int_(body, "completion_tokens");
}

} // namespace

/* Многоходовой слот.
 *
 * Слот заполняется ТОЛЬКО если у хоста есть поле llm_chat_messages. Это
 * единственное изменение переноса, и до него деградация была написана
 * дважды: этот слот заполнялся ВСЕГДА, а внутри него жил собственный
 * фолбэк на llm_complete_ex (старый src/plugin_main.cpp:346-360).
 *
 * Чем это было плохо, измерено на живом коде:
 *   - второй экземпляр решения «историю заменяет последняя реплика»
* рядом с единственным (core/llm_source.cpp:24), то есть ровно тот
     * класс расхождения, который стоил D2 и D12; llm_source.h при этом
 *     объявляет себя ЕДИНСТВЕННЫМ местом запасного пути, и объявление
 *     было ложью: ядро всегда получало непустой llm_chat и до своего
 *     фолбэка не доходило никогда;
 *   - ни одна строка этого кода не попадала в сборку тестов (src/ в неё
 *     не входит), то есть весь слой деградации не был проверен ничем;
 *   - отмена и таймаут в этой ветке отсутствовали, тогда как у
 *     llm_chat_messages их даёт call_llm_chat_messages (И6.6).
 *
 * Теперь слот пуст, и ядро само уходит на одногилый путь — с честной
 * потерей истории и метрик, потому что они там и должны теряться: ответ
 * без usage нельзя превратить в «0 токенов», это выдумка (И11.9).
 */
void install_llm_chat(HostCallbacks& cb, const LlamaHostApi* api,
                      LlamaPluginHost* host) {
    /* Проверяем, поддерживает ли хост llm_chat_messages (по size). */
    if (!has_llm_chat_messages(api)) return;

    const LlamaHostApi* api_snapshot = api;
    LlamaPluginHost* host_snapshot = host;

    cb.llm_chat = [api_snapshot, host_snapshot](
                      const std::string& sys_prompt,
                      const std::vector<ModelMessage>& messages,
                      LlmReply& out) -> bool {
        if (!api_snapshot || !host_snapshot) return false;

        /* JSON-массив сообщений собирает единственная реализация
         * (host_bridge/llm_stream_shim.cpp): стриминговый путь пользуется
         * ею же, и две копии разошлись бы при первом же новом поле
         * реплики. */
        const std::string json = messages_to_json(messages);

        /* 2.1: Retry для транзиентных ошибок LLM (429/5xx/timeout).
         * Хост сам ретраит облачные вызовы (OpenRouterClient::complete),
         * этот цикл — страховка для остальных путей (локальный сервер,
         * сетевые сбои), которые хост возвращает как ok=0/nullptr.
         *
         * И12.5: «повторяемо ли» решает ОДИН классификатор
         * (core/llm_error.h), а не свой список подстрок здесь. Свой
         * список был вторым разбором того же текста, что идёт в
         * LlmEvent::provider_error, — то есть D12: два места, которые
         * разъедутся. Они и разъехались бы: старый список считал
         * «исчерпан» (контекст закончился) повторяемым, потому что
         * стоял в одной строке с «429», а повтор запроса, который не
         * помещается в контекст, не поместится и в третий раз.
         *
         * Сам цикл ретраев (задержки, попытки, заголовки retry-after,
         * обратный отсчёт в UI) заменяется в И12.6; здесь он читает
         * готовый признак, чтобы тот не остался мёртвым полем.
         *
         * И12.6: политика повторов вынесена в core/retry.h и задаётся
         * числами из плана — initial 2000 мс, фактор 2, джиттер 0.25,
         * максимум 5 попыток.
         *
         * «Повторяемо ли» решает классификатор И12.5 (core/llm_error.h) —
         * единственный, кто разбирает текст отказа.
         *
         * ГЛАВНОЕ ОГРАНИЧЕНИЕ, и оно НЕ про этот цикл. Цикл живёт на
         * блокирующем пути (cb.llm_chat), а ход агента на стриминге идёт
         * через LlmClient, где повторов нет вовсе. То есть политика
         * защищает сводщика и старый хост без стриминга. Записано
         * отклонением 208, чтобы это не приняли за «повторы работают». */
        const RetryPolicy kRetryPolicy;

        for (int attempt = 0; attempt < kRetryPolicy.max_attempts; ++attempt) {
            /* И6.6: вызов хоста отменён быть не может (поля
             * llm_chat_messages_cancel в ABI нет) — останавливается только
             * ожидание. Раньше здесь стоял std::async, чей future в
             * деструкторе БЛОКИРУЕТ до конца задачи, то есть «Стоп» и
             * «таймаут» ждали провайдера целиком; и ответ никогда не
             * освобождался. Оба дефекта закрыты в host_bridge/llm_blocking
             * и проверяются тестом — дефект, который не видно, обязан
             * быть вынесен туда, где его можно проверить. */
            const int timeout_ms = engine().state().llm_timeout_ms;
            std::string resp;
            const BlockingOutcome outcome = call_llm_chat_messages(
                api_snapshot, host_snapshot, text::sanitize_utf8(sys_prompt), json,
                []() { return engine().state().abort_requested.load(); },
                timeout_ms, resp);

            if (outcome == BlockingOutcome::Aborted) {
                out.error = "Прервано пользователем";
                return false;
            }
            if (outcome == BlockingOutcome::TimedOut) {
                if (!has_llm_chat_messages(api_snapshot)) {
                    /* Хост старый: блокирующего вызова у него нет. Не
                     * «таймаут» — это правда о хосте, и притворяться
                     * иначе означало бы искать причину не там. */
                    out.error = "Хост не поддерживает блокирующий вызов LLM";
                    return false;
                }
                out.error = "Таймаут LLM-вызова (" +
                            std::to_string(timeout_ms / 1000) + " с)";
                return false;
            }
            /* Строка хоста уже освобождена в call_llm_chat_messages и
             * вернулась копией: ни strdup, ни free_string здесь больше
             * не нужны, и двойного освобождения быть не может. */
            if (json::int_(resp, "ok") == 1) {
                parse_chat_reply(resp, out);
                return true;
            }

            std::string error_text = json::str(resp, "error");
            if (error_text.empty()) error_text = "LLM-ошибка (ok=0)";

            /* И12.5: вид и повторяемость — одним классификатором. */
            const ProviderFailure failure =
                classify_provider_failure(error_text);
            if (should_retry(kRetryPolicy, failure.retryable, attempt)) {
                /* И12.6: заголовок `retry-after` через ABI недостижим
                 * (отдаётся только `{"ok":0,"error":…}`), но поле
                 * читается: хост, который его отдаст, начнёт управлять
                 * паузами без правок здесь. -1 — «сервер ничего не
                 * сказал», и тогда работает расчётная задержка. */
                const int server_ms = json::int_(resp, "retry_after_ms");
                const int delay_ms = retry_delay_ms_random(
                    kRetryPolicy, attempt, server_ms > 0 ? server_ms : -1);
                engine().push_event(AgentEvent::Retry,
                                    retry_notice(error_text, failure.kind, attempt,
                                                 kRetryPolicy.max_attempts, delay_ms));
                /* Спим с проверкой abort — «Стоп» прерывает ожидание.
                 * Шаг 100 мс оставлен: при задержке до минуты ждать без
                 * проверки означало бы, что кнопка не работает всё это
                 * время. */
                const auto& st = engine().state();
                for (int waited = 0;
                     waited < delay_ms && !st.abort_requested.load();
                     waited += 100) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (st.abort_requested.load()) {
                    out.error = "Прервано пользователем";
                    return false;
                }
                continue;
            }
            out.error = error_text;
            return false;
        }
        out.error = "LLM: все попытки исчерпаны";
        return false;
    };
}

void install_llm_slots(HostCallbacks& cb, const LlamaHostApi* api,
                       LlamaPluginHost* host) {
    if (!api || !host) return;

    cb.llm_is_connected = [api, host]() -> bool {
        return api->llm_is_connected(host) == 1;
    };

    /* Одногилый слот. Заполняется ВСЕГДА, в том числе у хоста без
     * llm_chat_messages: это и есть запасной путь старого хоста, и
     * выбирает его ядро (core/llm_source.cpp), а не этот файл. */
    cb.llm_complete = [api, host](const std::string& sys, const std::string& user,
                                  std::string& resp) -> bool {
        char* r = nullptr;
        int rc = api->llm_complete_ex(host, sys.c_str(), user.c_str(), &r);
        if (rc != 1 || !r) return false;
        resp = r;
        api->free_string(host, r);
        return true;
    };

    /* И6.4: потоковый путь. Слот заполняется ТОЛЬКО если у хоста есть поле
     * llm_chat_stream; на старом хосте он остаётся пустым, и это не ошибка —
     * вызывающий обязан уйти на блокирующий llm_chat. Проверка один раз при
     * инициализации: она читает структуру хоста, а тот живёт всё время работы
     * плагина и не меняет version.
     *
     * ВАЖНО: колбэки живут дольше вызова, поэтому вызывающий обязан дождаться
     * on_done до возврата из ll_plugin_shutdown — хост делает dlclose сразу
     * после вызова, и колбэк в выгруженный код читает освобождённую память. */
    if (has_llm_chat_stream(api)) {
        cb.llm_chat_stream = [api, host](
                                 const std::string& sys_prompt,
                                 const std::vector<ModelMessage>& messages,
                                 const std::string& request_json,
                                 std::function<void(void*)> on_started,
                                 std::function<void(const char*, int)> on_delta,
                                 std::function<void(const char*, const char*, const char*)> on_tool_delta,
                                 std::function<void(const std::string&)> on_done) -> bool {
            return call_llm_chat_stream(api, host, sys_prompt, messages,
                                        request_json, on_started, on_delta,
                                        on_tool_delta, on_done);
        };
        /* И6.6: отмена настоящая только если хост умеет. На старом хосте
         * слот пустой, и LlmClient называет это ограничение вслух. */
        if (has_llm_chat_cancel(api)) {
            cb.llm_chat_cancel = [api](void* handle) {
                cancel_llm_chat_stream(api, handle);
            };
        }
    }

    /* И13.5: символы файла через AST хоста. Слот заполняется ТОЛЬКО если у
     * хоста есть поле llm_ast_symbols; на старом хосте он остаётся пустым, и
     * обзор проекта уходит на строковый скан — core/ast_symbols.cpp называет
     * это вслух в самом обзоре, чтобы деградация не выглядела полным
     * обзором. Проба один раз при инициализации, как у стриминга. */
    if (has_llm_ast_symbols(api)) {
        cb.ast_symbols = [api, host](const std::string& path,
                                     const std::string& language) {
            return call_ast_symbols(api, host, path, language);
        };
    }

    install_llm_chat(cb, api, host);
}

} // namespace host
} // namespace coder