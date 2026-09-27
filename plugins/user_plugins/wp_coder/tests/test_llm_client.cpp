/*
 * test_llm_client.cpp — единый вход к LLM с живой доставкой (И6.5).
 *
 * Главное свойство здесь не «события приходят», а «оба пути дают одну и ту же
 * последовательность». Проверяется сравнением: результат стримингового хода
 * сверяется с блокирующим на том же финальном ответе. Если разойдутся, то
 * незаметно: агент на одном пути закроет задачу текстом, а на другом
 * выполнит инструмент.
 */

#include "core/llm_client.h"

#include "core/llm_source.h"
#include "test_framework.h"

#include <string>
#include <vector>

using namespace coder;

namespace {

/* Что «модель» присылает в этом прогоне. */
struct Script {
    bool start = true;                        /* хост согласился на поток */
    std::vector<std::pair<std::string, int>> deltas;  /* (текст, вид) */
    std::vector<std::string> tool_frags;       /* "call_id|имя|фрагмент" */
    std::string done_json;
    bool never_finishes = false;              /* хост не зовёт on_done */
    /* Колбэки сохраняются, чтобы тест мог доиграть поток ПОСЛЕ возврата:
     * без этого флаг «ход брошен» нечем было бы проверить — хост, который
     * не зовёт on_done, по определению молчит и в дельтах. */
    bool capture_callbacks = false;
};

using DeltaFn = std::function<void(const char*, int)>;
using ToolFn = std::function<void(const char*, const char*, const char*)>;
using DoneFn = std::function<void(const std::string&)>;

Script* g_script = nullptr;

/* Фиктивный handle: хост сообщает его через on_started до входа в запрос. */
int g_fake_handle = 0;

/* Сохранённые колбэки последнего хода — см. тест про таймаут. */
DeltaFn g_late_delta;
ToolFn g_late_tool;
DoneFn g_late_done;

/* Хост-колбэк: повторяет поведение моста И6.4, но без C-ABI. */
bool fake_stream(const std::string&, const std::vector<ModelMessage>&,
                 const std::string&, std::function<void(void*)> on_started,
                 DeltaFn on_delta, ToolFn on_tool_delta, DoneFn on_done)
{
    if (!g_script || !g_script->start) return false;
    if (on_started) on_started(&g_fake_handle);
    if (g_script->capture_callbacks) {
        g_late_delta = on_delta;
        g_late_tool = on_tool_delta;
        g_late_done = on_done;
    }
    for (const auto& d : g_script->deltas) on_delta(d.first.c_str(), d.second);
    for (const auto& t : g_script->tool_frags) {
        const size_t a = t.find('|');
        const size_t b = t.find('|', a + 1);
        on_tool_delta(t.substr(0, a).c_str(), t.substr(a + 1, b - a - 1).c_str(),
                      t.substr(b + 1).c_str());
    }
    if (g_script->never_finishes) return true;
    on_done(g_script->done_json);
    return true;
}

HostCallbacks callbacks_with_stream() {
    HostCallbacks cb;
    cb.llm_chat_stream = fake_stream;
    return cb;
}

HostCallbacks callbacks_without_stream() {
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&,
                     LlmReply& out) -> bool {
        out.content = "текст";
        out.finish_reason = "stop";
        return true;
    };
    return cb;
}

/* Экранирование обязательно: хост собирает этот JSON сериализатором, и сырой
 * перевод строки внутри строки сделал бы JSON невалидным. Заглушка без
 * экранирования «работала» бы ровно до того ответа, где в тексте модели
 * оказался перевод строки, — то есть проверяла бы не то. */
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

std::string ok_json(const std::string& content, const char* finish = "stop") {
    return std::string("{\"ok\":1,\"content\":\"") + json_escape(content) +
           "\",\"finish_reason\":\"" + finish +
           "\",\"prompt_tokens\":10,\"completion_tokens\":5}";
}

/* Имена видов одной строкой: ASSERT_EQ печатает значения в поток, а
 * вектор строк печатать нечем, поэтому сравнение векторов здесь просто не
 * собралось бы. */
std::string names_of(const std::vector<LlmEvent>& events) {
    std::string out;
    for (const LlmEvent& e : events) {
        if (!out.empty()) out += ",";
        out += e.kind_name();
    }
    return out;
}

} // namespace

/* --- Авторитетная последовательность --- */

/* Оба пути обязаны дать один и тот же ход. Это и есть смысл «единого
 * разбора протокола»: сверка, а не доверие. */
TEST(streaming_and_blocking_agree_on_the_answer) {
    const std::string content = "Просто ответ без вызова.";

    Script script;
    script.deltas = {{content, 0}};
    script.done_json = ok_json(content);
    g_script = &script;
    std::vector<LlmEvent> streamed;
    LlmClient::fetch(callbacks_with_stream(), "sys", {{"user", "x"}}, streamed,
                     [](const LlmEvent&) {}, 1000);
    g_script = nullptr;

    HostCallbacks blocking = callbacks_without_stream();
    blocking.llm_chat = [&](const std::string&, const std::vector<ModelMessage>&,
                            LlmReply& out) -> bool {
        out.content = content;
        out.finish_reason = "stop";
        /* Метрики — те же, что в on_done: иначе сверка проверяла бы не
         * совпадение путей, а разницу тестовых заглушек. */
        out.prompt_tokens = 10;
        out.completion_tokens = 5;
        return true;
    };
    std::vector<LlmEvent> direct;
    llm_source::fetch(blocking, "sys", {{"user", "x"}}, direct);

    ASSERT_EQ(names_of(streamed), names_of(direct));
    ASSERT_TRUE(!names_of(streamed).empty());
    const LlmResponse a = llm_source::fold(streamed);
    const LlmResponse b = llm_source::fold(direct);
    ASSERT_EQ(a.text(), b.text());
    ASSERT_EQ(a.finish_reason(), b.finish_reason());
    ASSERT_EQ(a.usage().input, b.usage().input);
    ASSERT_EQ(a.usage().output, b.usage().output);
}

/* Тот же ответ, но модель написала блок вызова. Поток присылает его как
 * текст, и это не должно просочиться в собранный ход: иначе вызов
 * потерялся бы, а в тексте ответа остался бы мусор. */
TEST(tool_block_is_cut_from_the_streamed_answer_too) {
    /* Блок огорожен, как его пишет модель по протоколу: extract_action
     * ищет "```wp_action". Голый wp_action{...} не извлекается, и такой
     * тест проверял бы не то. */
    const std::string content =
        "смотрю\n```wp_action\n{\"tool\":\"read_file\",\"path\":\"a.txt\"}\n```";

    Script script;
    script.deltas = {{"смотрю\n```wp_action\n", 0},
                     {"{\"tool\":\"read_file\",\"path\":\"a.txt\"}", 0},
                     {"\n```", 0}};
    script.done_json = ok_json(content, "tool-calls");
    g_script = &script;
    std::vector<LlmEvent> streamed;
    const bool ok = LlmClient::fetch(callbacks_with_stream(), "sys",
                                     {{"user", "x"}}, streamed,
                                     [](const LlmEvent&) {}, 1000);
    g_script = nullptr;

    ASSERT_TRUE(ok);
    const LlmResponse response = llm_source::fold(streamed);
    ASSERT_EQ(response.tool_calls().size(), size_t(1));
    ASSERT_EQ(response.tool_calls()[0].name, std::string("read_file"));
    /* Блока в тексте быть не должно: он уехал в вызов. */
    ASSERT_TRUE(response.text().find("wp_action") == std::string::npos);
    ASSERT_TRUE(response.text().find("```") == std::string::npos);
    ASSERT_EQ(response.text(), std::string("смотрю"));
}

/* --- Живая доставка --- */

/* Предпросмотр приходит по мере поступления, а не всё сразу. Проверяется
 * тем, что события есть ДО закрытия хода: иначе «стриминг» был бы
 * блокирующим с лишним шагом. */
TEST(preview_events_arrive_before_the_turn_closes) {
    Script script;
    script.deltas = {{"При", 0}, {"вет", 0}};
    script.done_json = ok_json("Привет");
    g_script = &script;

    std::vector<std::string> seen;
    std::vector<LlmEvent> events;
    LlmClient::fetch(callbacks_with_stream(), "sys", {{"user", "x"}}, events,
                     [&](const LlmEvent& e) {
                         seen.push_back(e.kind_name());
                         /* Пока ход не закрыт, авторитетного хода ещё нет. */
                         ASSERT_EQ(events.size(), size_t(0));
                     }, 1000);
    g_script = nullptr;

    ASSERT_TRUE(seen.size() >= 3);
    ASSERT_EQ(seen[0], std::string("text_start"));
    ASSERT_EQ(seen[1], std::string("text_delta"));
    /* Последнее событие предпросмотра — text_end: открытый блок обязан быть
     * закрыт, иначе UI ждал бы блок, который никто не закроет. */
    ASSERT_EQ(seen.back(), std::string("text_end"));
}

/* Размышление в предпросмотре идёт отдельным видом и не смешивается с
 * текстом: иначе рассуждение попало бы на экран как слова ответа. */
TEST(reasoning_arrives_as_its_own_kind) {
    Script script;
    script.deltas = {{"Думаю", 1}, {"Ответ", 0}};
    script.done_json = ok_json("Ответ");
    g_script = &script;

    std::vector<LlmEvent> preview;
    std::vector<LlmEvent> events;
    LlmClient::fetch(callbacks_with_stream(), "sys", {{"user", "x"}}, events,
                     [&](const LlmEvent& e) { preview.push_back(e); }, 1000);
    g_script = nullptr;

    bool saw_reasoning_start = false;
    bool saw_reasoning_delta = false;
    for (const LlmEvent& e : preview) {
        if (e.is(LlmEventKind::ReasoningStart)) saw_reasoning_start = true;
        if (e.is(LlmEventKind::ReasoningDelta)) saw_reasoning_delta = true;
    }
    ASSERT_TRUE(saw_reasoning_start);
    ASSERT_TRUE(saw_reasoning_delta);
    const LlmResponse response = llm_source::fold(events);
    ASSERT_EQ(response.text(), std::string("Ответ"));
    ASSERT_EQ(response.reasoning(), std::string(""));
}

/* Куски аргументов приходят в предпросмотре с идентификатором и именем, и
 * блок закрывается. */
TEST(tool_fragments_are_previewed_with_names_and_closed) {
    Script script;
    script.tool_frags = {"call_0|read_file|{", "call_0|read_file|}"};
    script.done_json = ok_json("", "tool-calls");
    g_script = &script;

    std::vector<LlmEvent> preview;
    std::vector<LlmEvent> events;
    LlmClient::fetch(callbacks_with_stream(), "sys", {{"user", "x"}}, events,
                     [&](const LlmEvent& e) { preview.push_back(e); }, 1000);
    g_script = nullptr;

    /* Имя несёт ToolInputStart и только он: у дельты поля имени нет по
     * устройству модели событий (И5.1), и провайдер присылает имя один раз —
     * в первом фрагменте. Проверяем, что имя долетело и не выдумано. */
    bool start = false, end = false;
    int deltas = 0;
    for (const LlmEvent& e : preview) {
        if (e.is(LlmEventKind::ToolInputStart)) {
            start = true;
            ASSERT_EQ(e.tool_name(), std::string("read_file"));
        }
        if (e.is(LlmEventKind::ToolInputDelta)) ++deltas;
        if (e.is(LlmEventKind::ToolInputEnd)) end = true;
    }
    ASSERT_TRUE(start);
    ASSERT_TRUE(end);
    ASSERT_EQ(std::to_string(deltas), std::string("2"));
}

/* --- Ошибки и откат --- */

/* Отказ провайдера обязан дойти до вызывающего как ProviderError, а не как
 * пустой успешный ход. */
TEST(provider_error_becomes_a_provider_error_event) {
    Script script;
    script.deltas = {{"начало", 0}};
    script.done_json = "{\"ok\":0,\"error\":\"HTTP 429: лимит\"}";
    g_script = &script;

    std::vector<LlmEvent> events;
    const bool ok = LlmClient::fetch(callbacks_with_stream(), "sys",
                                     {{"user", "x"}}, events,
                                     [](const LlmEvent&) {}, 1000);
    g_script = nullptr;

    ASSERT_FALSE(ok);
    const LlmResponse response = llm_source::fold(events);
    ASSERT_FALSE(response.error().empty());
    ASSERT_TRUE(response.error().find("429") != std::string::npos);
}

/* Хост отказался запускать поток: ход не теряется, а уходит на блокирующий
 * путь. Иначе отказ хоста означал бы потерю хода. */
TEST(refused_stream_falls_back_to_blocking) {
    Script script;
    script.start = false;
    g_script = &script;

    HostCallbacks cb = callbacks_with_stream();
    bool blocking_called = false;
    cb.llm_chat = [&](const std::string&, const std::vector<ModelMessage>&,
                      LlmReply& out) -> bool {
        blocking_called = true;
        out.content = "ответ";
        out.finish_reason = "stop";
        return true;
    };

    std::vector<LlmEvent> events;
    const bool ok = LlmClient::fetch(cb, "sys", {{"user", "x"}}, events,
                                     [](const LlmEvent&) {}, 1000);
    g_script = nullptr;

    ASSERT_TRUE(ok);
    ASSERT_TRUE(blocking_called);
    ASSERT_EQ(llm_source::fold(events).text(), std::string("ответ"));
}

/* Хост без стриминга: события всё равно доставляются в on_event, иначе
 * вызывающему пришлось бы иметь две ветки поведения, и одна была бы
 * забыта. */
TEST(old_host_still_delivers_events_to_the_callback) {
    std::vector<std::string> seen;
    std::vector<LlmEvent> events;
    const bool ok = LlmClient::fetch(callbacks_without_stream(), "sys",
                                     {{"user", "x"}}, events,
                                     [&](const LlmEvent& e) {
                                         seen.push_back(e.kind_name());
                                     }, 1000);
    ASSERT_TRUE(ok);
    ASSERT_EQ(std::to_string(seen.size()), std::to_string(events.size()));
    ASSERT_TRUE(!seen.empty());
}

/* Пустой on_event НЕ отключает стриминг: иначе хост, умеющий только поток,
 * оказался бы непригоден вызывающему, которому события не нужны, — молча
 * уйдя на блокирующий путь, мы бы не заметили, что хост его не умеет.
 * Ход от этого не меняется: тот же разбор, тот же результат. */
TEST(no_callback_still_uses_the_stream) {
    Script script;
    script.deltas = {{"Привет", 0}};
    script.done_json = ok_json("Привет");
    g_script = &script;
    std::vector<LlmEvent> events;
    const bool ok = LlmClient::fetch(callbacks_with_stream(), "sys",
                                     {{"user", "x"}}, events, nullptr, 1000);
    g_script = nullptr;

    ASSERT_TRUE(ok);
    ASSERT_EQ(llm_source::fold(events).text(), std::string("Привет"));
    /* Блокирующий колбэк у такого хоста отсутствует, поэтому успех здесь
     * возможен только через поток. */
    HostCallbacks only_stream = callbacks_with_stream();
    ASSERT_FALSE(static_cast<bool>(only_stream.llm_chat));
}

/* Хост, который не закрыл поток, не должен вешать агента навсегда. */
TEST(unclosed_stream_times_out_with_an_error) {
    Script script;
    script.deltas = {{"При", 0}};
    script.never_finishes = true;
    g_script = &script;

    std::vector<LlmEvent> events;
    const bool ok = LlmClient::fetch(callbacks_with_stream(), "sys",
                                     {{"user", "x"}}, events,
                                     [](const LlmEvent&) {}, 120);
    g_script = nullptr;

    ASSERT_FALSE(ok);
    const LlmResponse response = llm_source::fold(events);
    ASSERT_TRUE(response.error().find("таймаут") != std::string::npos);
}

/* После таймаута поток может ещё присылать дельты — отмены у хоста нет
 * (это И6.6). Доставка обязана прекратиться: иначе модель допишет текст в UI
 * уже после того, как цикл ушёл на следующий шаг, и пользователь увидит два
 * ответа вперемешку.
 *
 * «Хвост» доигрывается вручную: хост, который не зовёт on_done, по
 * определению молчит и в дельтах, поэтому иначе флаг «ход брошен» нечем
 * было бы проверить.
 *
 * Чего этот тест НЕ ловит: захват turn ПО ССЫЛКЕ в колбэках. Это
 * use-after-free, но при брошенном ходе первым делом читается abandoned,
 * и чтение атомарного флага не инструментируется даже под ASan — до
 * инструментированной памяти управление просто не доходит (проверено: весь
 * набор под -fsanitize=address,undefined с detect_leaks=1 не сообщает
 * ничего). Защита здесь — форма кода, то есть [turn] вместо [&], плюс
 * комментарий в llm_client.cpp, а не эта проверка. */
TEST(nothing_is_delivered_after_a_timeout) {
    Script script;
    script.deltas = {{"При", 0}};
    script.never_finishes = true;
    script.capture_callbacks = true;
    g_script = &script;

    int delivered = 0;
    std::vector<LlmEvent> events;
    const bool ok = LlmClient::fetch(callbacks_with_stream(), "sys",
                                     {{"user", "x"}}, events,
                                     [&](const LlmEvent&) { ++delivered; }, 120);
    const int before_timeout = delivered;
    g_script = nullptr;

    ASSERT_FALSE(ok);
    ASSERT_TRUE(before_timeout > 0);

    /* Поздние куски — то, что модель дописала после нашего таймаута. */
    ASSERT_TRUE(static_cast<bool>(g_late_delta));
    g_late_delta("вет, поздно", 0);
    g_late_tool("call_0", "read_file", "{}");
    g_late_done("{\"ok\":1,\"content\":\"Привет\",\"finish_reason\":\"stop\"}");

    ASSERT_EQ(std::to_string(delivered), std::to_string(before_timeout));
    g_late_delta = nullptr;
    g_late_tool = nullptr;
    g_late_done = nullptr;
}
