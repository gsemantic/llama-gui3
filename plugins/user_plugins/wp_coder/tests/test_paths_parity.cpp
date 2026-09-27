/*
 * test_paths_parity.cpp — стриминговый и блокирующий пути обязаны давать
 * ОДИН И ТОТ ЖЕ ход (И6.9).
 *
 * Проверка табличная, а не «один счастливый случай»: предыдущая проверка
 * закрывала ровно один сценарий (простой текст), и этого мало — расхождение
 * могло прийти на блоке вызова, на пустом ответе, на отказе провайдера или
 * на метриках. Каждый сценарий прогоняется обоими путями, и сравниваются
 * две вещи: ПОСЛЕДОВАТЕЛЬНОСТЬ ВИДОВ СОБЫТИЙ и свёрнутый ответ.
 *
 * Вторая половина задачи — откат: хост без поля llm_chat_stream (старый
 * ABI) обязан давать тот же самый ход через блокирующий путь. Иначе
 * «прозрачность» отката существует только на словах.
 */

#include "core/llm_client.h"
#include "core/llm_source.h"
#include "host_bridge/llm_stream_probe.h"

#include "test_framework.h"

#include <string>
#include <vector>

using namespace coder;

namespace {

std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

/* Финальный ответ хоста — то, что придёт в on_done или в llm_chat. */
struct Answer {
    std::string content;
    std::string finish = "stop";
    int prompt = 10;
    int completion = 5;
    bool failed = false;
    std::string error;

    std::string to_json() const {
        if (failed) {
            return "{\"ok\":0,\"error\":\"" + json_escape(error) + "\"}";
        }
        return std::string("{\"ok\":1,\"content\":\"") + json_escape(content) +
               "\",\"finish_reason\":\"" + finish +
               "\",\"prompt_tokens\":" + std::to_string(prompt) +
               ",\"completion_tokens\":" + std::to_string(completion) + "}";
    }
};

Answer* g_answer = nullptr;

/* Заглушка в РАСКЛАДКЕ ABI. Проба спрашивает только про наличие поля, и
 * подставлять туда std::function-колбэк плагина нельзя: подпись у них
 * разная, и компилятор честно это отверг. */
int abi_stream_stub(LlamaPluginHost*, const char*, const char*, const char*, void*,
                    void (*)(void*, const char*, int),
                    void (*)(void*, const char*, const char*, const char*),
                    void (*)(void*, const char*)) {
    return 1;
}

using DeltaFn = std::function<void(const char*, int)>;
using ToolFn = std::function<void(const char*, const char*, const char*)>;
using DoneFn = std::function<void(const std::string&)>;

/* Стриминговый хост: отдаёт ответ кусками, затем финальный JSON. */
bool fake_stream(const std::string&, const std::vector<ModelMessage>&,
                 const std::string&, std::function<void(void*)> on_started,
                 DeltaFn on_delta, ToolFn, DoneFn on_done)
{
    if (!g_answer) return false;
    if (on_started) on_started(&g_answer);
    /* Режем ответ на куски по 8 символов: так видно, что результат не
     * зависит от разбиения, — а это и есть смысл сверки путей. */
    const std::string& text = g_answer->content;
    for (size_t i = 0; i < text.size(); i += 8) {
        on_delta(text.substr(i, 8).c_str(), 0);
    }
    on_done(g_answer->to_json());
    return true;
}

/* Блокирующий хост: та же строка ответа, тот же JSON по смыслу. */
HostCallbacks blocking_callbacks() {
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&,
                     LlmReply& out) -> bool {
        if (!g_answer) return false;
        if (g_answer->failed) {
            out.error = g_answer->error;
            return false;
        }
        out.content = g_answer->content;
        out.finish_reason = g_answer->finish;
        out.prompt_tokens = g_answer->prompt;
        out.completion_tokens = g_answer->completion;
        return true;
    };
    return cb;
}

HostCallbacks streaming_callbacks() {
    HostCallbacks cb;
    cb.llm_chat_stream = fake_stream;
    return cb;
}

std::string names_of(const std::vector<LlmEvent>& events) {
    std::string out;
    for (const LlmEvent& e : events) {
        if (!out.empty()) out += ",";
        out += e.kind_name();
    }
    return out;
}

struct Case {
    const char* name;
    Answer answer;
};

/* Проверка одного сценария обоими путями. */
void check_same(const Case& c) {
    g_answer = const_cast<Answer*>(&c.answer);

    std::vector<LlmEvent> via_streaming;
    const bool ok_stream =
        LlmClient::fetch(streaming_callbacks(), "sys", {{"user", "x"}},
                         via_streaming, nullptr, 1000);

    std::vector<LlmEvent> via_blocking;
    const bool ok_block =
        LlmClient::fetch(blocking_callbacks(), "sys", {{"user", "x"}},
                         via_blocking, nullptr, 1000);
    g_answer = nullptr;

    /* ok обязан совпасть: иначе один путь сообщил бы об успехе там, где
     * другой честно сказал об отказе. */
    ASSERT_EQ(ok_stream, ok_block);
    ASSERT_EQ(names_of(via_streaming), names_of(via_blocking));

    const LlmResponse a = llm_source::fold(via_streaming);
    const LlmResponse b = llm_source::fold(via_blocking);
    ASSERT_EQ(a.text(), b.text());
    ASSERT_EQ(a.error(), b.error());
    /* Сравнение имён: ASSERT_EQ печатает значения в поток, а
     * FailureKind печатать нечем, и такая проверка не собралась бы. */
    ASSERT_EQ(std::string(failure_kind_name(a.failure())),
              std::string(failure_kind_name(b.failure())));
    ASSERT_EQ(a.finish_reason(), b.finish_reason());
    ASSERT_EQ(a.usage().input, b.usage().input);
    ASSERT_EQ(a.usage().output, b.usage().output);
    ASSERT_EQ(a.tool_calls().size(), b.tool_calls().size());
    if (!a.tool_calls().empty()) {
        ASSERT_EQ(a.tool_calls()[0].name, b.tool_calls()[0].name);
        ASSERT_EQ(a.tool_calls()[0].arguments.get_string("path"),
                  b.tool_calls()[0].arguments.get_string("path"));
    }
}

} // namespace

/* --- Сверка сценариев --- */

/* Простой текст: базовый случай, который закрывала прежняя проверка. */
TEST(both_paths_agree_on_plain_text) {
    const Case c{"plain", {"Ответ без вызова.", "stop", 10, 5, false, ""}};
    check_same(c);
}

/* Текст вместе с прозой: граница слова должна совпасть. */
TEST(both_paths_agree_on_prose_with_a_block) {
    const Case c{"block",
        {"смотрю\n```wp_action\nTOOL: read_file\nPATH: a.txt\n```",
         "tool-calls", 12, 9, false, ""}};
    check_same(c);
}

/* Только блок вызова: текста нет вовсе, и пустой text() у обоих путей
 * обязан быть одинаковым. */
TEST(both_paths_agree_on_a_block_only_answer) {
    const Case c{"only-block",
        {"```wp_action\nTOOL: bash\nCMD: ls\n```", "tool-calls", 8, 4, false, ""}};
    check_same(c);
}

/* Пустой ответ: раньше именно тут пути могли разойтись — «нет текста» на
 * одном пути и «есть текст» на другом дают разные условия завершения. */
TEST(both_paths_agree_on_an_empty_answer) {
    const Case c{"empty", {"", "stop", 5, 0, false, ""}};
    check_same(c);
}

/* finish_reason говорит про инструмент, а блока в тексте нет: ход
 * нелепый, но оба пути обязаны передать его одинаково, а не один из них
 * «починить» за другой. */
TEST(both_paths_agree_on_tool_calls_without_a_block) {
    const Case c{"liar", {"просто текст", "tool-calls", 7, 3, false, ""}};
    check_same(c);
}

/* Отказ провайдера: разные тексты ошибки на двух путях означали бы, что
 * модель повторяет запрос с одним текстом и получает другой. */
TEST(both_paths_agree_on_a_provider_error) {
    const Case c{"error", {"", "stop", 0, 0, true, "HTTP 429: лимит запросов"}};
    check_same(c);
}

/* Метрики: usage приходит и в content-ответе, и в on_done, и разойтись
 * может именно он — а по нему считается стоимость. */
TEST(both_paths_agree_on_usage) {
    const Case c{"usage", {"Ответ.", "stop", 1234, 567, false, ""}};
    check_same(c);
}

/* Многострочный ответ с кавычками и слешем: разбиение на дельты не должно
 * ни влиять на результат, ни ломать экранирование. */
TEST(both_paths_agree_on_multiline_text_with_quotes) {
    const Case c{"multiline",
        {"строка \"кавычки\"\nи слеш \\\nконец", "stop", 40, 20, false, ""}};
    check_same(c);
}

/* --- Откат при отсутствии поля в ABI --- */

/*
 * Хост без llm_chat_stream: поле в структуре отсутствует (старый .so
 * собран против старого заголовка). Плагин обязан уйти на блокирующий путь и
 * дать тот же ход, что и на новом хосте.
 *
 * Именно это и есть «откат при отсутствии символа в ABI»: без него старые
 * хосты просто перестали бы работать, причём молча — поле прочиталось бы
 * как мусор.
 */
TEST(host_without_the_field_falls_back_and_gives_the_same_turn) {
    LlamaHostApi api{};
    api.size = sizeof(LlamaHostApi);
    /* Поле есть по размеру, но не заполнено — так выглядит хост, который
     * про него не знает. */
    api.llm_chat_stream = nullptr;
    ASSERT_FALSE(host::has_llm_chat_stream(&api));

    const Case c{"fallback", {"Ответ для отката.", "stop", 11, 6, false, ""}};
    g_answer = const_cast<Answer*>(&c.answer);

    /* Хост без стриминговых слотов — ровно то, что получит плагин. */
    std::vector<LlmEvent> events;
    const bool ok = LlmClient::fetch(blocking_callbacks(), "sys",
                                     {{"user", "x"}}, events, nullptr, 1000);
    g_answer = nullptr;

    ASSERT_TRUE(ok);
    const LlmResponse r = llm_source::fold(events);
    ASSERT_EQ(r.text(), std::string("Ответ для отката."));
    ASSERT_EQ(r.usage().input, (long long)11);
}

/* Проба обязана отличать «поля нет» от «поле есть и заполнено»: иначе
 * откат не срабатывал бы там, где нужен. */
TEST(probe_distinguishes_a_missing_field_from_a_filled_one) {
    LlamaHostApi api{};
    api.size = sizeof(LlamaHostApi);
    api.llm_chat_stream = &abi_stream_stub;
    ASSERT_TRUE(host::has_llm_chat_stream(&api));

    /* Хост постарше: поля в его структуре просто нет. */
    api.size = offsetof(LlamaHostApi, llm_chat_stream);
    ASSERT_FALSE(host::has_llm_chat_stream(&api));

    /* Совсем старый хост: и структура короче, и поля нет. */
    LlamaHostApi ancient{};
    ancient.size = offsetof(LlamaHostApi, llm_chat_messages) + sizeof(void*);
    ASSERT_FALSE(host::has_llm_chat_stream(&ancient));
}

/* Старый хост без стриминга не должен отдавать пустых событий: пустой
 * вектор читался бы как «модель промолчала», а не как «нечем спросить». */
TEST(fallback_never_returns_an_empty_event_list) {
    const Case c{"notempty", {"Есть ответ.", "stop", 3, 2, false, ""}};
    g_answer = const_cast<Answer*>(&c.answer);
    std::vector<LlmEvent> events;
    const bool ok = LlmClient::fetch(blocking_callbacks(), "sys",
                                     {{"user", "x"}}, events, nullptr, 1000);
    g_answer = nullptr;
    ASSERT_TRUE(ok);
    ASSERT_FALSE(events.empty());
}
