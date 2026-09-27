// llm_blocking.cpp — блокирующий вызов хоста, переживающий отмену (И6.6).

#include "llm_blocking.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace coder {
namespace host {
namespace {

/*
 * Состояние вызова и владение его результатом.
 *
 * Мьютекс здесь — не излишество. Схема владения «кто первый забрал» на одних
 * атомиках требует перепроверки после каждой записи и всё равно допускает
 * окно, где обе стороны считают себя владельцами. С мьютексом обе проверки
 * не могут пройти одновременно, и владение единственно по построению.
 */
struct BlockingCall {
    std::mutex mtx;
    std::condition_variable cv;

    bool done = false;
    /* Результат живёт в поле, а не в локальной переменной потока: поток
     * возвращается раньше, чем его результат заберут. */
    char* raw = nullptr;
    /* Результат забрали (потребитель) или от него отказались (вызывающий
     * ушёл по отмене/таймауту). Истина означает: raw освобождать некому,
     * кроме потока-производителя. */
    bool claimed = false;

    /* Единственная точка, где строка хоста освобождается. */
    void release_raw(const LlamaHostApi* api, LlamaPluginHost* host)
    {
        if (!raw) return;
        /* host здесь может быть nullptr (тесты, да и вызов без хоста), и
         * требование непустого host не повод НЕ освобождать строку: утечка
         * не зависит от того, кто прислал указатель. */
        if (api && api->free_string) api->free_string(host, raw);
        raw = nullptr;
    }
};

} // namespace

BlockingOutcome call_llm_chat_messages(const LlamaHostApi* api,
                                       LlamaPluginHost* host,
                                       const std::string& sys_prompt,
                                       const std::string& messages_json,
                                       const std::function<bool()>& is_cancelled,
                                       int timeout_ms,
                                       std::string& out_json)
{
    if (!has_llm_chat_messages(api) || !api->llm_chat_messages) {
        out_json = "";
        return BlockingOutcome::TimedOut;
    }

    auto call = std::make_shared<BlockingCall>();

    /* Копии строк И САМОЙ ТАБЛИЦЫ HOST API: поток живёт дольше вызова, и
     * захват указателя на api означал бы чтение из ушедшего кадра. В
     * приложении api — статическая таблица и выжила бы, но полагаться на
     * это — значит прятать требование о времени жизни внутри функции, у
     * которой такого требования в подписи нет. (Поймано ASan: чтение
     * api->free_string из кадра уже вернувшегося вызова.)
     *
     * Таблица — набор указателей на функции, копия дешёвая, и поток от
     * неё больше не зависит никак. */
    const LlamaHostApi api_copy = *api;
    const std::string sys_copy = sys_prompt;
    const std::string json_copy = messages_json;

    /* detach, а НЕ std::async: деструктор future от std::async блокирует до
     * конца задачи, и весь смысл этого вызова — НЕ ждать до конца. */
    std::thread([call, api_copy, host, sys_copy, json_copy]() {
        char* raw = api_copy.llm_chat_messages(
            host, sys_copy.empty() ? nullptr : sys_copy.c_str(),
            json_copy.c_str());
        std::lock_guard<std::mutex> lk(call->mtx);
        call->raw = raw;
        call->done = true;
        /* Отменённый вызов результата не получит: освобождаем здесь. Иначе
         * каждый «Стоп» утекал бы на строку ответа. */
        if (call->claimed) call->release_raw(&api_copy, host);
        call->cv.notify_all();
    }).detach();

    const auto deadline = timeout_ms < 0
        ? std::chrono::time_point<std::chrono::steady_clock>::max()
        : std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    std::unique_lock<std::mutex> lk(call->mtx);
    /* Ожидание режется на короткие куски, и на каждом заново проверяется
     * признак отмены.
     *
     * Почему не одно wait_until до срока: признак отмены — обычная функция
     * (флаг abort_requested), а не condvar, и уведомления от него не будет.
     * Одно ожидание до срока проснулось бы только когда придёт ответ, то
     * есть «Стоп» снова ждал бы провайдера целиком — ровно тот дефект,
     * который здесь и чинится. */
    const auto slice = std::chrono::milliseconds(20);
    for (;;) {
        if (call->done) break;
        if (is_cancelled && is_cancelled()) break;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        call->cv.wait_until(lk, now + slice < deadline ? now + slice : deadline);
    }

    /* claimed = true: потребитель ровно один, и он забирает права на строку
     * безусловно — отмену мы тоже считаем отказом от неё, иначе право
     * осталось бы в воздухе и строку не освободил бы никто.
     *
     * Ветки «а вдруг claimed уже занят» здесь нет намеренно: взять claimed
     * до этого места может только этот же поток, так что проверка была бы
     * недостижимой — такой код только вводит читателя в заблуждение, что
     * владение тут спорное. */
    call->claimed = true;

    if (!call->done) {
        /* Отмена или таймаут, ответ ещё не готов: строка появится позже, и её
         * освободит поток-производитель, увидев claimed. */
        lk.unlock();
        out_json = "";
        return is_cancelled && is_cancelled() ? BlockingOutcome::Aborted
                                              : BlockingOutcome::TimedOut;
    }

    std::string result;
    if (call->raw) result.assign(call->raw);
    call->release_raw(api, host);
    lk.unlock();
    out_json = std::move(result);
    return BlockingOutcome::Answered;
}

} // namespace host
} // namespace coder
