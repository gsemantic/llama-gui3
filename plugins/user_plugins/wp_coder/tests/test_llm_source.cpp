/*
 * test_llm_source.cpp — И5.7: адаптер «ответ хоста → события».
 *
 * Здесь проверяется граница, на которой до И5.7 всё ломалось: ответ приходит
 * ОДНОЙ строкой, в которой текст модели и блок вызова инструмента не
 * отличимы. Адаптер обязан разрезать её на TextDelta и ToolCall — иначе цикл
 * пришлось бы знать про протокол wp_action, а именно это и было его
 * устройством до И5.7.
 *
 * Почему проверяется поток событий, а не «после fold всё то же самое».
 * Свёртка (И5.3) проверена отдельно и на потоках от настоящего провайдера.
 * Здесь важна другая вещь: какие СОБЫТИЯ появляются на границе. Ошибка
 * «блок уехал в текст» или «имя инструмента потерялось» даёт в свёртке
 * правдоподобный результат — просто неверный, — и заметить её можно
 * только по событиям.
 *
 * Отдельно проверяется, что неразобранный блок НЕ исчезает: без этого
 * модель увидела бы в истории пустой вызов и повторила бы его.
 */

#include "test_framework.h"
#include "test_printers.h"
#include "../core/llm_source.h"

#include <string>
#include <vector>

using namespace coder;

namespace {

/* Виды событий по порядку — компактный отпечаток потока. */
std::vector<std::string> kinds_of(const std::vector<LlmEvent>& events) {
    std::vector<std::string> out;
    for (const LlmEvent& e : events) out.push_back(e.kind_name());
    return out;
}

std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

/* Первый вызов в потоке (в тестах он всегда один: у текстового протокола
 * в строке помещается один блок вызова). */
const LlmToolCall& only_call(const LlmResponse& r) {
    return r.tool_calls().at(0);
}

LlmReply reply_with(const std::string& content,
                    const std::string& finish = "stop",
                    int prompt_tokens = 0, int completion_tokens = 0) {
    LlmReply r;
    r.ok = true;
    r.content = content;
    r.finish_reason = finish;
    r.prompt_tokens = prompt_tokens;
    r.completion_tokens = completion_tokens;
    return r;
}

size_t count_occurrences(const std::string& haystack,
                         const std::string& needle) {
    if (needle.empty()) return 0;
    size_t n = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

} // namespace

/* ======================================================================
 * Поток событий: форма
 * ====================================================================== */

TEST(llm_source_plain_text_is_one_text_delta) {
    std::vector<LlmEvent> events;
    llm_source::reply_to_events(reply_with("Готово: файл прочитан."), events);
    ASSERT_EQ(join(kinds_of(events), ","),
              std::string("step_start,text_start,text_delta,text_end,"
                          "step_finish,finish"));
    const LlmResponse r = llm_source::fold(events);
    ASSERT_EQ(r.text(), std::string("Готово: файл прочитан."));
    ASSERT_EQ(r.tool_calls().size(), (size_t)0);
    ASSERT_EQ(r.finish_reason(), std::string("stop"));
}

TEST(llm_source_json_block_becomes_a_tool_call) {
    std::vector<LlmEvent> events;
    llm_source::reply_to_events(reply_with(
        "Смотрю файл.\n```json\n{\"tool\": \"read_file\", \"path\": \"a.txt\"}\n```\nДальше подожду."),
        events);
    /* Блок обязан стать ВЫЗОВОМ, а не частью текста: текст без блока —
     * то, что модель сказала человеку, блок — то, что она вызвала. */
    ASSERT_EQ(join(kinds_of(events), ","),
              std::string("step_start,text_start,text_delta,text_end,"
                          "tool_input_start,tool_input_delta,tool_call,"
                          "tool_input_end,step_finish,finish"));
    const LlmResponse r = llm_source::fold(events);
    /* Текст остаётся таким, каким его написала модель: пустая строка перед
     * блоком — это её текст, а не мусор адаптера. Убран только хвост (тест
     * ниже проверяет, что и он не остаётся). */
    ASSERT_EQ(r.text(), std::string("Смотрю файл.\n\nДальше подожду."));
    ASSERT_EQ(r.tool_calls().size(), (size_t)1);
    ASSERT_EQ(only_call(r).name, std::string("read_file"));
    ASSERT_EQ(only_call(r).arguments.get_string("path"), std::string("a.txt"));
    ASSERT_TRUE(only_call(r).runnable());
    /* Сырой блок сохраняется: модель на следующем шаге должна увидеть тот
     * же текст, который написала сама (MessagePart::raw_call). */
    ASSERT_EQ(only_call(r).raw_input,
              std::string("{\"tool\": \"read_file\", \"path\": \"a.txt\"}\n"));
}

TEST(llm_source_legacy_block_is_parsed_by_the_adapter) {
    /* Legacy-форма не JSON, поэтому её обязан разобрать адаптер: свёртка
     * разбирает только JSON, и такой блок закрылся бы ошибкой. */
    std::vector<LlmEvent> events;
    llm_source::reply_to_events(reply_with(
        "Читаю.\n```wp_action\nTOOL: read_file\nPATH: main.py\n```"),
        events);
    const LlmResponse r = llm_source::fold(events);
    ASSERT_EQ(r.text(), std::string("Читаю."));
    ASSERT_EQ(r.tool_calls().size(), (size_t)1);
    ASSERT_EQ(only_call(r).name, std::string("read_file"));
    ASSERT_EQ(only_call(r).arguments.get_string("path"), std::string("main.py"));
    ASSERT_TRUE(only_call(r).runnable());
    /* Сырой блок хранится ровно таким, каким его вырезал протокол, — с
     * переводом строки перед закрывающими кавычками. Обрезка была бы
     * подменой текста модели своим. */
    ASSERT_EQ(only_call(r).raw_input,
              std::string("TOOL: read_file\nPATH: main.py\n"));
}

TEST(llm_source_block_only_leaves_no_text_part) {
    std::vector<LlmEvent> events;
    llm_source::reply_to_events(
        reply_with("```json\n{\"tool\": \"bash\", \"command\": \"ls\"}\n```"),
        events);
    const LlmResponse r = llm_source::fold(events);
    ASSERT_EQ(r.text(), std::string(""));
    ASSERT_EQ(r.tool_calls().size(), (size_t)1);
    ASSERT_EQ(only_call(r).name, std::string("bash"));
}

TEST(llm_source_unparsed_block_becomes_a_failed_call_not_silence) {
    std::vector<LlmEvent> events;
    llm_source::reply_to_events(
        reply_with("Сделаю так.\n```json\n{\"путь\": \"a.txt\"}\n```"),
        events);
    const LlmResponse r = llm_source::fold(events);
    ASSERT_EQ(r.tool_calls().size(), (size_t)1);
    /* Вызов остаётся ВИДНЫМ и закрытым ошибкой: молчание было бы хуже —
     * модель увидела бы в истории пустой вызов и повторила бы тот же
     * блок. При этом он не runnable, чтобы цикл не пытался его выполнить. */
    ASSERT_TRUE(only_call(r).failed);
    ASSERT_TRUE(only_call(r).finished);
    ASSERT_FALSE(only_call(r).runnable());
    ASSERT_TRUE(only_call(r).error.find("не удалось разобрать") !=
                std::string::npos);
    /* Сырой блок сохранён даже в неразобранном виде: по нему видно, что
     * модель написала на самом деле. */
    ASSERT_TRUE(only_call(r).raw_input.find("\"путь\"") != std::string::npos);
    /* Текст остаётся текстом: ошибка разбора не должна съесть прозу. */
    ASSERT_EQ(r.text(), std::string("Сделаю так."));
}

TEST(llm_source_finish_carries_usage_and_reason) {
    std::vector<LlmEvent> events;
    llm_source::reply_to_events(
        reply_with("ок", "length", 1200, 340), events);
    const LlmResponse r = llm_source::fold(events);
    ASSERT_EQ(r.finish_reason(), std::string("length"));
    ASSERT_EQ(r.usage().input, 1200LL);
    ASSERT_EQ(r.usage().output, 340LL);
    ASSERT_TRUE(r.usage().invariants_hold());
}

TEST(llm_source_text_survives_a_cyrillic_symbol) {
    /* Дельта целиком проходит через склейку по кодам символов (И5.3), и
     * кириллица не должна ни потеряться, ни превратиться в битые байты. */
    const std::string cyrillic = "Случай из «кавычек» и ёжика";
    std::vector<LlmEvent> events;
    llm_source::reply_to_events(reply_with(cyrillic), events);
    const LlmResponse r = llm_source::fold(events);
    ASSERT_EQ(r.text(), cyrillic);
}

/* ======================================================================
 * Запрос к хосту: единственное место с запасным путём
 * ====================================================================== */

TEST(llm_source_fetch_reports_failure_as_a_provider_error) {
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&,
                     LlmReply& out) {
        out.error = "сеть недоступна";
        return false;
    };
    std::vector<LlmEvent> events;
    const bool ok = llm_source::fetch(cb, "sys", {{kRoleUser, "привет"}}, events);
    ASSERT_FALSE(ok);
    /* Ошибка приходит событием, а не пустым вектором: вызывающий не обязан
     * знать, откуда взялась неудача. */
    ASSERT_EQ(events.size(), (size_t)1);
    ASSERT_TRUE(events[0].is(LlmEventKind::ProviderError));
    ASSERT_EQ(llm_source::fold(events).error(), std::string("сеть недоступна"));
}

TEST(llm_source_fetch_falls_back_to_a_single_shot_host) {
    std::string seen_prompt;
    std::string seen_user;
    int chat_calls = 0;
    HostCallbacks cb;
    cb.llm_chat = [&](const std::string&, const std::vector<ModelMessage>&,
                      LlmReply&) {
        ++chat_calls;
        return false;
    };
    /* Хост без llm_chat: у него есть только одногилый запрос, и он должен
     * получить последнюю реплику пользователя, а не весь диалог. */
    cb.llm_complete = [&](const std::string& sys, const std::string& user,
                          std::string& resp) {
        seen_prompt = sys;
        seen_user = user;
        resp = "Готово";
        return true;
    };
    HostCallbacks only_complete;
    only_complete.llm_complete = cb.llm_complete;

    std::vector<LlmEvent> events;
    const bool ok = llm_source::fetch(only_complete, "системный", {
        {kRoleUser, "первая"}, {kRoleAssistant, "ответ"}, {kRoleUser, "вторая"}},
        events);
    ASSERT_TRUE(ok);
    ASSERT_EQ(chat_calls, 0);
    ASSERT_EQ(seen_prompt, std::string("системный"));
    ASSERT_EQ(seen_user, std::string("вторая"));
    const LlmResponse r = llm_source::fold(events);
    ASSERT_EQ(r.text(), std::string("Готово"));
    /* Метрики одногилого хоста неизвестны, и выдумывать их нельзя: ноль
     * честнее, чем правдоподобное число. */
    ASSERT_TRUE(r.usage().empty());
}

TEST(llm_source_fetch_without_any_host_callbacks_reports_an_error) {
    HostCallbacks empty;
    std::vector<LlmEvent> events;
    const bool ok = llm_source::fetch(empty, "sys", {}, events);
    ASSERT_FALSE(ok);
    ASSERT_EQ(llm_source::fold(events).error(), std::string("не ответил"));
}

TEST(llm_source_fold_of_an_empty_stream_is_an_empty_response) {
    const LlmResponse r = llm_source::fold({});
    ASSERT_TRUE(r.empty());
    ASSERT_TRUE(r.ok());
}

/* ======================================================================
 * И5.10: сквозной путь — события → свёртка → ход → транскрипт
 *
 * Каждое звено проверено отдельно (свёртка — test_llm_events.cpp, модель
 * сообщений — test_message.cpp, цикл — test_agent_loop.cpp), но ломалось
 * оно именно ВМЕСТЕ: протокол попадал в транскрипт дважды, результат
 * оставался без вызова, а короткий ответ закрывал задачу. Сквозной тест
 * ловит именно эту комбинацию.
 * ====================================================================== */

TEST(llm_source_events_become_a_complete_model_turn) {
    std::vector<LlmEvent> events;
    llm_source::reply_to_events(reply_with(
        "Смотрю файл.\n```json\n{\"tool\": \"read_file\", \"path\": \"a.txt\"}\n```"),
        events);
    LlmResponse response = llm_source::fold(events);

    /* 1. Условие завершения видит ход с вызовом, а не финальный ответ —
     *    хотя хост прислал finish=stop. */
    Message turn = turn_to_message(response, "msg_000000000001");
    ASSERT_TRUE(turn.has_open_tool_part());
    ASSERT_EQ((int)turn_verdict(response, turn, "msg_000000000001"),
              (int)TurnVerdict::NeedsTools);

    /* 2. Вызов исполнен — часть закрыта, но ход всё равно не финальный.
     *    Условие 1 смотрит на то, чего ПРОСИЛ провайдер (finish=
     *    tool-calls), а не на то, пришёл ли уже результат: модель должна
     *    увидеть результат в следующем запросе и ответить на него, и
     *    закрывать задачу сейчас — значит оборвать переписку на полуслове. */
    LlmResponse::reduce(response, LlmEvent::tool_result(
        "call_0", ToolOutput()));
    sync_tool_parts(turn, response);
    ASSERT_FALSE(turn.has_open_tool_part());
    ASSERT_EQ((int)turn_verdict(response, turn, "msg_000000000001"),
              (int)TurnVerdict::NeedsTools);

    /* 3. Транскрипт для модели: вызов — реплика ассистента, результат —
     *    реплика пользователя, и ни одно из двух не потеряно. */
    std::vector<Message> history;
    history.push_back(Message::user("прочитай a.txt"));
    history.push_back(turn);
    const std::vector<ModelMessage> model = to_model_messages(history);
    ASSERT_EQ(model.size(), (size_t)3);
    ASSERT_EQ(model[0].role, std::string(kRoleUser));
    ASSERT_EQ(model[1].role, std::string(kRoleAssistant));
    ASSERT_TRUE(model[1].content.find("read_file") != std::string::npos);
    ASSERT_TRUE(model[1].content.find("Смотрю файл.") != std::string::npos);
    ASSERT_EQ(model[2].role, std::string(kRoleUser));
    ASSERT_TRUE(model[2].content.find("RESULT [read_file]:") != std::string::npos);
    /* И протокола в текстовой части нет — ровно один раз. */
    ASSERT_EQ(count_occurrences(model[1].content, "\"tool\""), (size_t)1);
}

TEST(llm_source_a_text_answer_completes_the_task) {
    /* Обратный край: ход без вызовов с длинным текстом закрывает задачу. */
    std::vector<LlmEvent> events;
    const std::string answer(400, 'x');
    llm_source::reply_to_events(reply_with(answer), events);
    const LlmResponse response = llm_source::fold(events);
    const Message turn = turn_to_message(response, "msg_000000000001");
    ASSERT_EQ(turn.parts.size(), (size_t)1);
    ASSERT_TRUE(turn_completes_task(response, turn, "msg_000000000001"));
    /* И текст ответа не потерян по дороге из ответа в сообщение. */
    ASSERT_EQ(turn.text(), answer);
}
