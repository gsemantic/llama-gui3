/*
 * test_llm_blocking.cpp — блокирующий вызов хоста при отмене (И6.6).
 *
 * Проверяется ровно то, что было сломано и не было видно:
 *
 *   1. std::async возвращает future, деструктор которого БЛОКИРУЕТ до конца
 *      задачи. Значит «Стоп» и «таймаут» ждали провайдера целиком. Проверка
 *      здесь: вызов должен ВЕРНУТСЯ по отмене, пока фейковый хост ещё спит.
 *   2. Ответ — malloc-строка хоста. Если её никто не забрал, она не
 *      освобождалась никогда. Проверка здесь: free_string обязан быть вызван
 *      в каждом исходе, включая отмену.
 *
 * Хост фейковый, поэтому тест показывает, что происходит, когда генерация
 * продолжается после нашего ухода, — а в приложении это ровно тот случай,
 * из-за которого «Стоп» и выглядел как «ничего не происходит».
 */

#include "host_bridge/llm_blocking.h"

#include "test_framework.h"

#include <atomic>
#include <deque>
#include <memory>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

using namespace coder;
using namespace coder::host;

namespace {

/* Что делает «хост» в этом прогоне. */
struct Behaviour {
    int sleep_ms = 0;              /* сколько «генерирует» */
    char* answer = nullptr;        /* что вернёт (владеет free_string) */
    std::atomic<int> free_calls{0};
    std::atomic<int> calls{0};
    /* «Хост закончил работу». Поток-производитель живёт дольше вызова —
     * это и есть смысл общего владения в реализации. */
    std::atomic<int> produced{0};
};

/* Состояние «хоста» передаётся через сам LlamaPluginHost* — то есть через
 * тот самый непрозрачный указатель, который ABI и так обязан передавать
 * плагину. Глобальная переменная здесь была бы гонкой: тесты идут
 * подряд, а поток предыдущего ещё живёт и переприсваивание указателя
 * читатель видит наполовину. */
Behaviour* behaviour_of(LlamaPluginHost* host)
{
    return reinterpret_cast<Behaviour*>(host);
}

/*
 * Состояние «хоста» живёт в куче и держится до конца прогона.
 *
 * Поток-производитель внутри call_llm_chat_messages detach-ится и живёт
 * дольше вызова — при отмене и таймауте это как раз и происходит. Состояние
 * на стеке теста к этому моменту уже мертво, и фейк читал бы освобождённый
 * кадр (проверено: сегфолт). Поэтому объект в куче, а ссылка на него
 * складывается в deque: добавление не перемещает и не инвалидирует уже
 * живущие элементы, в отличие от vector.
 */
std::deque<std::shared_ptr<Behaviour>> g_alive;

struct HostFixture {
    std::shared_ptr<Behaviour> behaviour;
    LlamaPluginHost* host;
};

HostFixture make_host(int sleep_ms, const char* answer = nullptr) {
    HostFixture f;
    f.behaviour = std::make_shared<Behaviour>();
    f.behaviour->sleep_ms = sleep_ms;
    f.behaviour->answer = const_cast<char*>(answer);
    f.host = reinterpret_cast<LlamaPluginHost*>(f.behaviour.get());
    g_alive.push_back(f.behaviour);
    return f;
}

char* fake_llm_chat_messages(LlamaPluginHost* host, const char*, const char*)
{
    Behaviour* b = behaviour_of(host);
    ++b->calls;
    if (b->sleep_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(b->sleep_ms));
    }
    /* Ответ хост выделяет сам — его и должен кто-то освободить. */
    const char* src = b->answer ? b->answer : "{\"ok\":0}";
    const size_t n = std::strlen(src) + 1;
    char* out = static_cast<char*>(std::malloc(n));
    std::memcpy(out, src, n);
    ++b->produced;
    return out;
}

void fake_free_string(LlamaPluginHost* host, char* s)
{
    ++behaviour_of(host)->free_calls;
    std::free(s);
}

/* Дождаться, пока «хост» закончит, и пока строка будет освобождена. */
void quiesce(const Behaviour* b) {
    for (int i = 0; i < 200 && b->produced.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (int i = 0; i < 200 && b->free_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

LlamaHostApi api_with_blocking() {
    LlamaHostApi api{};
    api.size = sizeof(LlamaHostApi);
    api.llm_chat_messages = &fake_llm_chat_messages;
    api.free_string = &fake_free_string;
    return api;
}

template <class Body>
int waited_ms(const Body& body) {
    const auto t0 = std::chrono::steady_clock::now();
    body();
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count());
}

} // namespace

/* Ответ доходит целиком и строка хоста освобождается. */
TEST(answered_call_returns_the_body_and_frees_it) {
    const HostFixture fx = make_host(0, "{\"ok\":1,\"content\":\"Привет\"}");
    const Behaviour& b = *fx.behaviour;
    const LlamaPluginHost* host = fx.host;
    const LlamaHostApi api = api_with_blocking();

    std::string out;
    const BlockingOutcome outcome = call_llm_chat_messages(
        &api, const_cast<LlamaPluginHost*>(host), "sys", "[]", []() { return false; }, 5000, out);

    ASSERT_TRUE(outcome == BlockingOutcome::Answered);
    ASSERT_EQ(out, std::string("{\"ok\":1,\"content\":\"Привет\"}"));
    ASSERT_EQ(b.free_calls.load(), 1);
}

/* Главная проверка: отмена возвращает управление СРАЗУ, а не когда «хост
 * догенерирует». Прокручивающийся дефект И2.2 именно в этом: время
 * возврата совпадало со временем ответа провайдера. */
TEST(cancel_returns_while_the_host_is_still_generating) {
    const HostFixture fx = make_host(1500);
    const Behaviour& b = *fx.behaviour;
    const LlamaPluginHost* host = fx.host;
    const LlamaHostApi api = api_with_blocking();

    std::atomic<bool> cancelled{false};
    std::string out;
    std::atomic<int> outcome_value{-1};
    const int ms = waited_ms([&] {
        std::thread([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            cancelled.store(true);
        }).detach();
        outcome_value.store(static_cast<int>(call_llm_chat_messages(
            &api, const_cast<LlamaPluginHost*>(host), "sys", "[]",
            [&] { return cancelled.load(); }, 30000, out)));
    });

    /* 1500 мс генерации, но вернуться обязаны заметно раньше. */
    ASSERT_TRUE(ms < 800);
    ASSERT_TRUE(ms >= 100);
    ASSERT_TRUE(static_cast<BlockingOutcome>(outcome_value.load()) ==
                BlockingOutcome::Aborted);
}

/* Ответ, дошедший после отмены, обязан быть освобождён — иначе каждый
 * «Стоп» утекал бы на строку ответа. */
TEST(late_answer_after_a_cancel_is_freed) {
    const HostFixture fx = make_host(300);
    const Behaviour& b = *fx.behaviour;
    const LlamaPluginHost* host = fx.host;
    const LlamaHostApi api = api_with_blocking();

    std::string out;
    const BlockingOutcome outcome = call_llm_chat_messages(
        &api, const_cast<LlamaPluginHost*>(host), "sys", "[]", []() { return true; }, 30000, out);

    ASSERT_TRUE(outcome == BlockingOutcome::Aborted);
    /* Ждём, пока «хост» догенерирует: освободить его строку должен поток. */
    quiesce(&b);
    ASSERT_EQ(b.free_calls.load(), 1);
    ASSERT_EQ(out, std::string(""));
}

/* Таймаут — не отмена: разные исходы, и вызывающий обязан их различать. */
TEST(timeout_is_not_a_cancel) {
    const HostFixture fx = make_host(1500);
    const Behaviour& b = *fx.behaviour;
    const LlamaPluginHost* host = fx.host;
    const LlamaHostApi api = api_with_blocking();

    std::string out;
    const int ms = waited_ms([&] {
        const BlockingOutcome outcome =
            call_llm_chat_messages(&api, const_cast<LlamaPluginHost*>(host), "sys", "[]",
                                   []() { return false; }, 120, out);
        ASSERT_TRUE(outcome == BlockingOutcome::TimedOut);
    });
    /* 120 мс таймаута против 1500 мс генерации. */
    ASSERT_TRUE(ms < 800);
    /* Строка, дошедшая после таймаута, обязана быть освобождена потоком. */
    quiesce(&b);
    ASSERT_EQ(b.free_calls.load(), 1);
}

/* Таймаут в 0 — «не ждать вовсе»: это честный вызов, а не зависание. */
TEST(zero_timeout_does_not_hang) {
    const HostFixture fx = make_host(500);
    const Behaviour& b = *fx.behaviour;
    const LlamaPluginHost* host = fx.host;
    const LlamaHostApi api = api_with_blocking();

    std::string out;
    const int ms = waited_ms([&] {
        call_llm_chat_messages(&api, const_cast<LlamaPluginHost*>(host), "sys", "[]",
                               []() { return false; }, 0, out);
    });
    ASSERT_TRUE(ms < 300);
}

/* Старый хост без блокирующего поля: это не таймаут, а отсутствие
 * возможности. Молчаливая подмена одного другим заставила бы искать
 * причину не там. */
TEST(old_host_is_reported_as_timed_out_with_an_empty_body) {
    const HostFixture fx = make_host(0);
    const Behaviour& b = *fx.behaviour;
    const LlamaPluginHost* host = fx.host;
    LlamaHostApi api = api_with_blocking();
    api.size = offsetof(LlamaHostApi, llm_chat_messages);
    api.llm_chat_messages = nullptr;

    std::string out = "мусор";
    const BlockingOutcome outcome =
        call_llm_chat_messages(&api, const_cast<LlamaPluginHost*>(host), "sys", "[]",
                               []() { return false; }, 100, out);
    ASSERT_TRUE(outcome == BlockingOutcome::TimedOut);
    ASSERT_EQ(out, std::string(""));
    ASSERT_EQ(b.calls.load(), 0);
}

/* Без срока ожидания (timeout < 0) отмена всё равно будит ожидание. */
TEST(no_deadline_still_wakes_on_cancel) {
    const HostFixture fx = make_host(2000);
    const Behaviour& b = *fx.behaviour;
    const LlamaPluginHost* host = fx.host;
    const LlamaHostApi api = api_with_blocking();

    std::atomic<bool> cancelled{false};
    std::string out;
    std::thread([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        cancelled.store(true);
    }).detach();

    const int ms = waited_ms([&] {
        call_llm_chat_messages(&api, const_cast<LlamaPluginHost*>(host), "sys", "[]",
                               [&] { return cancelled.load(); }, -1, out);
    });
    ASSERT_TRUE(ms < 800);
}

/* Ответ приходит ровно на границе отмены: кто-то должен забрать его, и
 * строка не должна остаться висеть. */
TEST(answer_racing_with_a_cancel_is_taken_exactly_once) {
    for (int i = 0; i < 20; ++i) {
        const HostFixture fx = make_host(0, "{\"ok\":1,\"content\":\"на грани\"}");
        const Behaviour& b = *fx.behaviour;
        const LlamaPluginHost* host = fx.host;
        const LlamaHostApi api = api_with_blocking();

        std::atomic<bool> cancelled{false};
        std::string out;
        const BlockingOutcome outcome = call_llm_chat_messages(
            &api, const_cast<LlamaPluginHost*>(host), "sys", "[]", [&] { return cancelled.load(); }, 5000, out);
    
        if (outcome == BlockingOutcome::Answered) {
            ASSERT_EQ(out, std::string("{\"ok\":1,\"content\":\"на грани\"}"));
        }
        /* Строка освобождена в любом исходе — ровно один раз. */
        quiesce(&b);
        ASSERT_EQ(b.free_calls.load(), 1);
        cancelled.store(true);
    }
}
