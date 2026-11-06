/*
 * test_timeline.cpp — И11.3: дерево вызовов инструментов по ходам.
 *
 * Предмет проверки — РЕШЕНИЯ, а не окно: виджет рисуется ImGui, тестового
 * харнесса для него нет, и 11.13 (golden-снапшоты рендера) сверяет
 * готовую строку и её вид. Поэтому всё, что можно решить без окна, лежит
 * в core/timeline.h и проверяется здесь; ui/coder_window.cpp остаётся
 * тонким слоем поверх — как в 11.1 и 11.2.
 *
 * Что проверяется и почему именно это:
 *
 *   1. ДЛИТЕЛЬНОСТЬ — единственный новый источник данных задачи. До И11.3
 *      в модели не было ни одного поля времени, поэтому проверяется не
 *      «время где-то есть», а три вещи, на которых поедет всё дерево:
 *      откуда берётся (шапка core/timeline.h, отклонение 152), что
 *      «неизвестно» не значит «ноль» и что время хода — сумма времён его
 *      вызовов, а не что-то одно выдуманное.
 *   2. ДЕРЕВО: ход — сообщение ассистента, внутри — его вызовы в порядке
 *      частей, якорь-реплика приходит из `parent_id`.
 *   3. СТАТУС хода выводится из статусов его вызовов, и порядок выбора
 *      назван: работающий вызов важнее отказа.
 *   4. ПОДПИСИ — готовые строки, потому что их сверяет 11.13.
 *
 * Фикстуры строятся НАСТОЯЩИМИ переходами состояния (set_running, сон,
 * set_result) там, где проверяется измерение, и restore_duration_ms там,
 * где нужна конкретная цифра: измерять настоящие две секунды ради подписи
 * смысла не имеет, а вот подмена времени вместо измерения — как раз тот
 * случай, который проверка обязана ловить. Поэтому измерение проверяется
 * отдельно (tests/test_message.cpp) и не подменяется здесь нигде.
 */

#include "../core/json_utils.h"
#include "../core/timeline.h"
/* И11.5: список нужных тел проверяется впрямую на снимках 11.4 — «нужно»
 * без «снимается» было бы обещанием. */
#include "../core/tool_display.h"

#include "test_framework.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace coder;

namespace {

/* Вызов, который отработал за `ms` миллисекунд: настоящий замер, а не
 * подставленная цифра. */
MessagePart done_call(const char* call_id, const char* tool, int ms) {
    MessagePart p = MessagePart::tool(call_id, tool);
    p.set_running();
    if (ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
    return p.set_result(ToolOutput());
}

/* Вызов с конкретной длительностью — для подписей, где важно ЧИСЛО, а не
 * способ его получения. Измерение проверено отдельно. */
MessagePart timed_call(const char* call_id, const char* tool,
                       long long duration_ms) {
    MessagePart p = MessagePart::tool(call_id, tool);
    p.restore_duration_ms(duration_ms);
    return p.set_result(ToolOutput());
}

Message user_message(const char* id, const std::string& text) {
    Message m = Message::user(text);
    m.id = id;
    return m;
}

Message assistant_message(const char* id, const std::string& parent_id,
                          std::vector<MessagePart> parts) {
    Message m = Message::assistant(parent_id);
    m.id = id;
    for (MessagePart& p : parts) m.parts.push_back(std::move(p));
    return m;
}

const timeline::Turn* find_turn(const timeline::Timeline& tl, size_t number) {
    for (const timeline::Turn& t : tl.turns) {
        if (t.number == number) return &t;
    }
    return nullptr;
}

timeline::Timeline of(std::vector<Message> history, size_t max_turns) {
    return timeline::build_timeline(timeline::timeline_source(history, max_turns));
}

/* Символы (кодовые точки) строки — счёт САМОЙ ПРОВЕРКИ, а не вызов
 * production-функции: ожидание, посчитанное тем же кодом, который
 * проверяют, не проверяет ничего. Правило простое: байт-продолжение UTF-8
 * имеет два старших бита 10, то есть `0x80`, и в символ не входит. */
size_t chars_of(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++n;
    }
    return n;
}

/* Идентификатор сообщения для фикстуры: у них длина фиксированная, потому
 * что `parent_id` ищется сравнением строк, а не по порядку. */
std::string msg_id(int n) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "msg_0000000000%02d", n);
    return buf;
}

/* Первая строка подписи, начинающаяся с `prefix`. Пусто — не нашлась. */
std::string line_with(const std::vector<std::string>& lines,
                      const std::string& prefix) {
    for (const std::string& l : lines) {
        if (l.rfind(prefix, 0) == 0) return l;
    }
    return std::string();
}

std::vector<std::string> call_labels(const timeline::Turn& turn) {
    std::vector<std::string> out;
    for (const timeline::Call& c : turn.calls) out.push_back(timeline::call_label(c));
    return out;
}

} // namespace

/* ======================================================================
 * Дерево: что является узлом и что в нём видно
 * ====================================================================== */

TEST(the_tree_groups_the_calls_of_a_turn_under_that_turn) {
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", "почини цикл"));
    h.push_back(assistant_message("msg_000000000002", "msg_000000000001",
                                  {done_call("call_0", "read_file", 2),
                                   done_call("call_1", "bash", 2)}));

    const timeline::Timeline tl = of(h, limits::kMaxTimelineTurns);
    ASSERT_EQ(tl.turns.size(), size_t(1));
    ASSERT_EQ(tl.calls_total, size_t(2));
    ASSERT_EQ(tl.omitted_turns, size_t(0));

    const timeline::Turn* turn = find_turn(tl, 1);
    ASSERT_TRUE(turn != nullptr);
    ASSERT_EQ(turn->id, std::string("msg_000000000002"));
    /* Порядок вызовов — порядок частей в сообщении, а не отсортированный
     * по имени: человек читает ход в том порядке, в каком он шёл. */
    ASSERT_EQ(turn->calls.size(), size_t(2));
    ASSERT_EQ(turn->calls[0].tool, std::string("read_file"));
    ASSERT_EQ(turn->calls[1].tool, std::string("bash"));
    /* Идентификатор вызова доезжает до окна: по нему окно отличает два
     * одинаковых `read_file` в одном ходе (оно же разводит их по
     * ImGui-идентификаторам). */
    ASSERT_EQ(turn->calls[0].call_id, std::string("call_0"));
    /* А не вызов — не строка дерева: текст хода рисует другая панель
     * (11.6), и текстовая часть в дереве выглядела бы вызовом, которого
     * не было. */
    for (const timeline::Call& c : turn->calls) {
        ASSERT_TRUE(!c.tool.empty());
    }
}

TEST(a_turn_is_the_assistant_message_and_not_the_human_reply) {
    /* Реплика человека — якорь, а не узел: узлом для каждой реплики был бы
     * ряд строк без вызовов. */
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", "сделай две вещи"));
    h.push_back(assistant_message("msg_000000000002", "msg_000000000001",
                                  {done_call("call_0", "read_file", 1)}));
    h.push_back(user_message("msg_000000000003", "и ещё одну"));
    h.push_back(assistant_message("msg_000000000004", "msg_000000000003",
                                  {done_call("call_1", "bash", 1)}));

    const timeline::Timeline tl = of(h, limits::kMaxTimelineTurns);
    ASSERT_EQ(tl.turns.size(), size_t(2));
    ASSERT_EQ(tl.turns[0].number, size_t(1));
    ASSERT_EQ(tl.turns[1].number, size_t(2));
    /* Якорь каждого хода — та реплика, на которую ОН отвечает, а не
     * последняя в истории: у хода, который был первым, его реплика. */
    ASSERT_TRUE(tl.turns[0].reply_to.find("две вещи") != std::string::npos);
    ASSERT_TRUE(tl.turns[1].reply_to.find("ещё одну") != std::string::npos);
}

TEST(the_number_of_a_turn_is_counted_over_the_whole_history) {
    /* Показ последних ходов не должен переименовывать тридцатый ход в
     * «Ход 1»: человек смотрит на дерево и сверяет его с тем, что было
     * раньше, и «Ход 1» у двух разных деревьев означал бы разное. */
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", "задача"));
    for (size_t i = 0; i < 5; ++i) {
        char call[32];
        std::snprintf(call, sizeof(call), "call_%d", static_cast<int>(i));
        h.push_back(assistant_message(msg_id(10 + static_cast<int>(i)).c_str(),
                                      "msg_000000000001",
                                      {done_call(call, "read_file", 0)}));
    }
    const timeline::Timeline tl = of(h, 2);
    ASSERT_EQ(tl.turns.size(), size_t(2));
    ASSERT_EQ(tl.omitted_turns, size_t(3));
    ASSERT_EQ(tl.turns[0].number, size_t(4));
    ASSERT_EQ(tl.turns[1].number, size_t(5));
}

TEST(the_turns_left_out_of_the_frame_are_counted) {
    /* Молча показанная часть дерева выглядела бы как «агент сделал вот
     * столько»: человек не узнал бы, что три хода выше не показаны. */
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", "задача"));
    for (size_t i = 0; i < 4; ++i) {
        h.push_back(assistant_message(msg_id(20 + static_cast<int>(i)).c_str(),
                                      "msg_000000000001", {}));
    }
    const timeline::Timeline tl = of(h, 3);
    ASSERT_EQ(tl.turns.size(), size_t(3));
    ASSERT_EQ(tl.omitted_turns, size_t(1));
}

TEST(only_the_newest_turn_is_open_by_default) {
    /* Открытые тридцать деревьев окно не показывают; закрытый последний
     * ход прячет ровно то, ради чего человек смотрит на дерево. */
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", "задача"));
    for (size_t i = 0; i < 3; ++i) {
        char call[32];
        std::snprintf(call, sizeof(call), "call_%d", static_cast<int>(i));
        h.push_back(assistant_message(msg_id(30 + static_cast<int>(i)).c_str(),
                                      "msg_000000000001",
                                      {done_call(call, "read_file", 0)}));
    }
    const timeline::Timeline tl = of(h, limits::kMaxTimelineTurns);
    ASSERT_EQ(tl.turns.size(), size_t(3));
    ASSERT_FALSE(tl.turns[0].default_open);
    ASSERT_FALSE(tl.turns[1].default_open);
    ASSERT_TRUE(tl.turns[2].default_open);
}

/* ======================================================================
 * Статус хода
 * ====================================================================== */

TEST(the_status_of_a_turn_is_derived_from_the_statuses_of_its_calls) {
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", "задача"));
    h.push_back(assistant_message("msg_000000000002", "msg_000000000001",
                                  {timed_call("call_0", "read_file", 10)}));
    MessagePart refused = done_call("call_1", "bash", 0);
    refused.set_error("[отказ] не разрешено");
    h.push_back(assistant_message("msg_000000000003", "msg_000000000001",
                                  {std::move(refused)}));
    h.push_back(assistant_message("msg_000000000004", "msg_000000000001",
                                  {MessagePart::tool("call_2", "bash")
                                       .set_running()}));
    const timeline::Timeline tl = of(h, limits::kMaxTimelineTurns);
    ASSERT_EQ(tl.turns.size(), size_t(3));
    /* Все вызовы успешны — это НЕ то же, что «вызовов не было»: стереть
     * разницу значило бы скрыть именно то, ради чего дерево рисуют. */
    ASSERT_EQ((int)tl.turns[0].status, (int)timeline::TurnStatus::Done);
    ASSERT_EQ((int)tl.turns[1].status, (int)timeline::TurnStatus::Failed);
    ASSERT_EQ((int)tl.turns[2].status, (int)timeline::TurnStatus::Working);
}

TEST(a_turn_with_a_refusal_and_a_still_running_call_is_working_not_failed) {
    /* Порядок выбора назван в шапке core/timeline.h: отказ одного вызова не
     * остановил ход, и называть ход ошибочным значило бы сказать человеку
     * то, чего не случилось. */
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", "задача"));
    MessagePart refused = done_call("call_0", "bash", 0);
    refused.set_error("[отказ] не разрешено");
    h.push_back(assistant_message("msg_000000000002", "msg_000000000001",
                                  {std::move(refused),
                                   MessagePart::tool("call_1", "write_file")
                                       .set_running()}));
    const timeline::Timeline tl = of(h, limits::kMaxTimelineTurns);
    ASSERT_EQ(tl.turns.size(), size_t(1));
    ASSERT_EQ((int)tl.turns[0].status, (int)timeline::TurnStatus::Working);
}

TEST(a_turn_with_a_call_that_has_not_started_yet_is_still_working) {
    /* Вызов может быть не начат (Pending), а не работать: блок вызова
     * разобран, но инструмент ещё не пошёл. Такой ход УЖЕ идёт, и назвать
     * его «только текст» значило бы утверждать, что вызовов не было. */
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", "задача"));
    h.push_back(assistant_message("msg_000000000002", "msg_000000000001",
                                  {MessagePart::tool("call_0", "read_file")}));
    const timeline::Timeline tl = of(h, limits::kMaxTimelineTurns);
    ASSERT_EQ(tl.turns.size(), size_t(1));
    ASSERT_EQ((int)tl.turns[0].calls[0].status, (int)ToolState::Pending);
    ASSERT_EQ((int)tl.turns[0].status, (int)timeline::TurnStatus::Working);
}

TEST(a_turn_that_only_answered_with_text_is_a_node_of_the_tree) {
    /* Иначе последний ход задачи — тот, где агент ответил текстом, — не
     * был бы виден никогда, и дерево обрывалось бы на полуслове. */
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", "задача"));
    h.push_back(assistant_message("msg_000000000002", "msg_000000000001",
                                  {MessagePart::text("готово")}));
    const timeline::Timeline tl = of(h, limits::kMaxTimelineTurns);
    ASSERT_EQ(tl.turns.size(), size_t(1));
    ASSERT_EQ((int)tl.turns[0].status, (int)timeline::TurnStatus::Text);
    ASSERT_EQ(tl.turns[0].calls.size(), size_t(0));
    ASSERT_EQ(tl.calls_total, size_t(0));
    ASSERT_TRUE(tl.turns[0].reply_known);
}

/* ======================================================================
 * Длительность: единственный новый источник данных задачи
 * ====================================================================== */

TEST(the_duration_of_a_turn_is_the_sum_of_the_durations_of_its_calls) {
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", "задача"));
    h.push_back(assistant_message("msg_000000000002", "msg_000000000001",
                                  {timed_call("call_0", "read_file", 1200),
                                   timed_call("call_1", "bash", 3000)}));
    const timeline::Timeline tl = of(h, limits::kMaxTimelineTurns);
    ASSERT_EQ(tl.turns[0].duration_ms, 4200LL);
    ASSERT_EQ(tl.duration_ms, 4200LL);
    /* Подпись называет, ЧТО это за время: время вызовов, а не «длительность
     * хода», потому что время запроса к модели в неё не входит и взять его
     * негде (шапка core/timeline.h). Назвать сумму «длительностью хода» без
     * оговорки значило бы записать в дерево то, чего в нём нет. */
    ASSERT_TRUE(timeline::turn_label(tl.turns[0]).find("время вызовов") !=
                std::string::npos);
}

TEST(an_unknown_duration_is_not_a_zero) {
    /* Ноль означал бы «отработал мгновенно» — правдоподобная ложь. Вызовов
     * без измеренного времени считается отдельно, и подпись говорит, сколько
     * их: сумма известных времен — нижняя оценка. */
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", "задача"));
    h.push_back(assistant_message("msg_000000000002", "msg_000000000001",
                                  {timed_call("call_0", "read_file", 1500),
                                   MessagePart::tool("call_1", "bash")
                                       .set_error("[ошибка] сломалось")}));
    const timeline::Timeline tl = of(h, limits::kMaxTimelineTurns);
    ASSERT_EQ(tl.turns[0].untimed_calls, size_t(1));
    ASSERT_EQ(tl.untimed_calls, size_t(1));
    ASSERT_EQ(tl.turns[0].duration_ms, 1500LL);
    const std::string label = timeline::turn_label(tl.turns[0]);
    ASSERT_TRUE(label.find("1.5 с") != std::string::npos);
    ASSERT_TRUE(label.find("+ 1 без времени") != std::string::npos);
    /* Тот же ход целиком: пропущенное время не теряется и в заголовке. */
    ASSERT_TRUE(timeline::timeline_head(tl).find("+ 1 без времени") !=
                std::string::npos);
}

TEST(a_turn_whose_calls_were_never_timed_shows_no_time_and_not_a_zero) {
    /* Ноль секунд у хода, у которого время не измерели, — это утверждение,
     * которого никто не делал. */
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", "задача"));
    h.push_back(assistant_message("msg_000000000002", "msg_000000000001",
                                  {MessagePart::tool("call_0", "read_file")
                                       .set_result(ToolOutput())}));
    const timeline::Timeline tl = of(h, limits::kMaxTimelineTurns);
    ASSERT_TRUE(tl.turns[0].duration_ms < 0);
    ASSERT_TRUE(tl.duration_ms < 0);
    const std::string label = timeline::turn_label(tl.turns[0]);
    ASSERT_TRUE(label.find("время вызовов: —") != std::string::npos);
    ASSERT_TRUE(label.find("+ 1 без времени") != std::string::npos);
    /* А измеренный ноль — законное время и выглядит как ноль: иначе «не
     * измерено» и «мгновенно» были бы одним и тем же словом. */
    std::vector<Message> zero;
    zero.push_back(user_message("msg_000000000001", "задача"));
    zero.push_back(assistant_message("msg_000000000002", "msg_000000000001",
                                     {timed_call("call_0", "read_file", 0)}));
    const timeline::Timeline tl0 = of(zero, limits::kMaxTimelineTurns);
    ASSERT_EQ(tl0.turns[0].duration_ms, 0LL);
    ASSERT_TRUE(timeline::duration_text(0) != timeline::duration_text(-1));
}

TEST(a_call_that_waited_for_a_person_is_timed_from_the_moment_it_started) {
    /* Отвергнутый вариант «мерять только работу инструмента» показал бы на
     * этом вызове те же 20 мс: интервал до set_error лежит ВНУТРИ ToolRunner,
     * где цикл ждёт разрешения. Вывод один и тот же, а отличаются они
     * ровно тем, что человек видел на экране минуту. */
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", "задача"));
    MessagePart waited = done_call("call_0", "bash", 25);
    waited.set_error("[отказ] пользователь не разрешил");
    h.push_back(assistant_message("msg_000000000002", "msg_000000000001",
                                  {std::move(waited)}));
    const timeline::Timeline tl = of(h, limits::kMaxTimelineTurns);
    ASSERT_EQ(tl.turns[0].calls.size(), size_t(1));
    ASSERT_TRUE(tl.turns[0].calls[0].duration_ms >= 25);
    /* Подпись вызова говорит и статус, и время: обе вещи нужны, чтобы
     * понять, почему вызов занял столько. */
    const std::string label = timeline::call_label(tl.turns[0].calls[0]);
    ASSERT_TRUE(label.find("ошибка") != std::string::npos);
    ASSERT_TRUE(label.rfind("bash: ", 0) == 0);
}

/* ======================================================================
 * Подписи: их сверяет 11.13
 * ====================================================================== */

TEST(every_state_of_a_call_has_its_own_word_and_its_own_colour) {
    /* Четыре состояния части и четыре подписи: своего перечислятеля
     * статусов у таймлайна нет (Д2, Д12), но и подпись у состояния одна.
     * Подпись «completed» была бы протокольной строкой в окне. */
    timeline::Call c;
    c.tool = "read_file";
    c.call_id = "call_0";

    c.status = ToolState::Pending;
    c.duration_ms = -1;
    const std::string pending = timeline::call_label(c);
    c.status = ToolState::Running;
    const std::string running = timeline::call_label(c);
    c.status = ToolState::Completed;
    c.duration_ms = 340;
    const std::string completed = timeline::call_label(c);
    c.status = ToolState::Error;
    const std::string failed = timeline::call_label(c);

    ASSERT_EQ(pending, std::string("read_file: не начат, —"));
    ASSERT_EQ(running, std::string("read_file: работает, —"));
    ASSERT_EQ(completed, std::string("read_file: готов, 340 мс"));
    ASSERT_EQ(failed, std::string("read_file: ошибка, 340 мс"));

    /* Цвета различаются: иначе состояние читалось бы только по слову, а в
     * свёрнутом дереве, где подпись может не помещаться, цвет — это всё,
     * что остаётся. Сравнение попарное и по-своему: проверка «цвета не
     * совпадают» на СПИСКЕ копий сравнивала бы разные объекты и прошла бы
     * при любом коде. */
    const timeline::StatusColor c_pending = timeline::status_color(ToolState::Pending);
    const timeline::StatusColor c_running = timeline::status_color(ToolState::Running);
    const timeline::StatusColor c_done = timeline::status_color(ToolState::Completed);
    const timeline::StatusColor c_failed = timeline::status_color(ToolState::Error);
    const timeline::StatusColor all[4] = {c_pending, c_running, c_done, c_failed};
    for (size_t i = 0; i < 4; ++i) {
        for (size_t j = i + 1; j < 4; ++j) {
            const bool same = all[i].r == all[j].r && all[i].g == all[j].g &&
                              all[i].b == all[j].b;
            if (same) {
                std::cerr << "  состояния " << i << " и " << j
                          << " покрашены одинаково" << std::endl;
            }
            ASSERT_TRUE(!same);
        }
    }
}

TEST(the_time_is_written_the_way_a_person_reads_it) {
    ASSERT_EQ(timeline::duration_text(-1), std::string("—"));
    ASSERT_EQ(timeline::duration_text(0), std::string("0 мс"));
    ASSERT_EQ(timeline::duration_text(1), std::string("1 мс"));
    ASSERT_EQ(timeline::duration_text(999), std::string("999 мс"));
    ASSERT_EQ(timeline::duration_text(1000), std::string("1.0 с"));
    ASSERT_EQ(timeline::duration_text(1250), std::string("1.2 с"));
    /* Целые, без плавающей точки: одна и та же длительность обязана
     * печататься одинаково и в окне, и в проверке. */
    ASSERT_EQ(timeline::duration_text(1999), std::string("1.9 с"));
    ASSERT_EQ(timeline::duration_text(59999), std::string("59.9 с"));
    ASSERT_EQ(timeline::duration_text(60000), std::string("1 мин 00 с"));
    ASSERT_EQ(timeline::duration_text(125000), std::string("2 мин 05 с"));
    /* Час — не редкость: вызов, на котором человек ушёл от компьютера,
     * обязан быть читаем, а не показываться как «7199.4 с». */
    ASSERT_EQ(timeline::duration_text(3600000), std::string("1 ч 00 мин"));
    ASSERT_EQ(timeline::duration_text(3900000), std::string("1 ч 05 мин"));
}

TEST(the_label_of_a_turn_names_its_number_state_counters_and_reply) {
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", "почини цикл"));
    h.push_back(assistant_message("msg_000000000002", "msg_000000000001",
                                  {timed_call("call_0", "read_file", 1500),
                                   timed_call("call_1", "bash", 2500)}));
    const timeline::Timeline tl = of(h, limits::kMaxTimelineTurns);
    ASSERT_EQ(timeline::turn_label(tl.turns[0]),
              std::string("Ход 1: готов, вызовов: 2, время вызовов: 4.0 с — "
                          "почини цикл"));
}

TEST(a_turn_whose_reply_is_gone_says_so_instead_of_showing_nothing) {
    /* Это бывает по-настоящему: компакшн (И7.7) подменяет старую историю
     * сводкой, и `parent_id` указывает на сообщение, которого в сессии
     * больше нет. Пустой якорь читался бы как «ход без запроса», то есть
     * как несуществующий ход. */
    std::vector<Message> h;
    h.push_back(assistant_message("msg_000000000009", "msg_000000000001",
                                  {timed_call("call_0", "read_file", 100)}));
    const timeline::Timeline tl = of(h, limits::kMaxTimelineTurns);
    ASSERT_EQ(tl.turns.size(), size_t(1));
    /* `parent_id` непустой, но такого сообщения в истории нет: якорь не
     * найден, и подпись обязана сказать об этом, а не показать пустоту,
     * похожую на пустую реплику. */
    /* `parent_id` в фикстуре НЕПУСТОЙ, а сообщения с таким идентификатором
     * в истории нет: если бы «найдена» означала «родитель непустой», эта
     * проверка прошла бы на коде, который ищет якорь по одному признаку
     * строки и никогда не смотрит в историю. */
    ASSERT_TRUE(!tl.turns[0].reply_known);
    ASSERT_TRUE(tl.turns[0].reply_to.empty());
    ASSERT_TRUE(timeline::turn_label(tl.turns[0]).find("реплика человека не найдена") !=
                std::string::npos);

    /* Реплика, которая есть, но пустая, — другое сообщение, и она тоже
     * называется: у пустой реплики и у потерянной причины разная. */
    std::vector<Message> empty;
    empty.push_back(user_message("msg_000000000001", ""));
    empty.push_back(assistant_message("msg_000000000002", "msg_000000000001",
                                      {timed_call("call_0", "read_file", 100)}));
    const timeline::Timeline tle = of(empty, limits::kMaxTimelineTurns);
    ASSERT_TRUE(tle.turns[0].reply_known);
    ASSERT_TRUE(timeline::turn_label(tle.turns[0]).find("реплика человека пуста") !=
                std::string::npos);
}

TEST(the_reply_of_a_turn_is_the_first_line_and_is_cut_at_the_limit) {
    /* Якорь нужен, чтобы «Ход 3» стал «Ход 3: почини тест», но полная
     * реплика — это часто много экранов текста. Обрезка помечается, иначе
     * человек решил бы, что агент понял задачу целиком. */
    /* Русская буква — ДВА байта, и это ровно то, что проверяется. Строка
     * СОБИРАЕТСЯ из UTF-8-константы повторением, а не символьным литералом:
     * литерал с кириллицей — многознаковая константа, и `std::string(n, c)`
     * берёт из неё один байт (то есть половину буквы), то есть набил бы
     * строку битым текстом, который обрез потом «починил» бы — и проверка
     * прошла бы на тексте, которого в сессии не бывает. */
    const std::string kYa = "\xD1\x8F";
    std::string long_line;
    for (size_t i = 0; i < limits::kMaxTimelineReplyChars + 50; ++i) {
        long_line += kYa;
    }
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001",
                             long_line + "\nи вторая строка"));
    h.push_back(assistant_message("msg_000000000002", "msg_000000000001",
                                  {timed_call("call_0", "read_file", 100)}));
    const timeline::Timeline tl = of(h, limits::kMaxTimelineTurns);
    ASSERT_TRUE(tl.turns[0].reply_clipped);
    /* Предел — в СИМВОЛАХ. Русский текст в UTF-8 занимает два байта на
     * букку, и обрез по байтам разрезал бы букву пополам: в подписи дерева
     * появился бы битый байт, а человек увидел бы «кракозябры» вместо
     * многоточия. */
    /* В ДАННЫХ — ровно предел символов, без знаков: текст реплики не должен
     * нести оформление подписи, иначе другое место показа (11.4) не
     * отличит обрезанную реплику от целой, написавшей «…» сама. */
    ASSERT_EQ(chars_of(tl.turns[0].reply_to), limits::kMaxTimelineReplyChars);
    /* И обрезанный текст остаётся целым UTF-8 — это и есть проверка на
     * «букву не разрезали». */
    ASSERT_TRUE(text::is_valid_utf8(tl.turns[0].reply_to));
    /* А знак «показано не всё» ставит ПОДПИСЬ, и вот она его содержит. */
    const std::string label = timeline::turn_label(tl.turns[0]);
    ASSERT_TRUE(label.find("\xE2\x80\xA6") != std::string::npos);
    ASSERT_TRUE(label.find(long_line.substr(0, 20)) != std::string::npos);
    /* Вторая строка реплики в подпись не попадает: ход отвечает на всю
     * реплику, а подпись — это первая строка. */
    ASSERT_TRUE(tl.turns[0].reply_to.find("вторая") == std::string::npos);

    /* Короткая реплика не обрезается и многоточия не получает. */
    std::vector<Message> short_h;
    short_h.push_back(user_message("msg_000000000001", "почини тест"));
    short_h.push_back(assistant_message("msg_000000000002", "msg_000000000001",
                                         {timed_call("call_0", "bash", 100)}));
    const timeline::Timeline tls = of(short_h, limits::kMaxTimelineTurns);
    ASSERT_EQ(tls.turns[0].reply_to, std::string("почини тест"));
    ASSERT_FALSE(tls.turns[0].reply_clipped);
}

/* ======================================================================
 * Границы проекции
 * ====================================================================== */

TEST(a_broken_byte_in_the_reply_is_repaired_and_the_repair_does_not_hide_the_cut) {
    /* Здесь ловушка, на которую проверка «обрезано» наступила: починенный
     * битый байт занимает три байта вместо одного, поэтому результат обреза
     * оказывается ДЛИННЕ исходника, и сравнение длин сказало бы «обрезания
     * не было». Признак обязан приходить из самого обреза. */
    std::string broken(limits::kMaxTimelineReplyChars + 20, '\xD1');
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", broken));
    h.push_back(assistant_message("msg_000000000002", "msg_000000000001",
                                  {timed_call("call_0", "read_file", 100)}));
    const timeline::Timeline tl = of(h, limits::kMaxTimelineTurns);
    ASSERT_TRUE(tl.turns[0].reply_known);
    ASSERT_TRUE(tl.turns[0].reply_clipped);
    ASSERT_TRUE(text::is_valid_utf8(tl.turns[0].reply_to));
    ASSERT_EQ(chars_of(tl.turns[0].reply_to), limits::kMaxTimelineReplyChars);
    /* Многоточие в подписи есть даже там, где текст был битым: иначе
     * «показано не всё» потерялось бы ровно на самом сломанном тексте. */
    ASSERT_TRUE(timeline::turn_label(tl.turns[0]).find(
                    std::string("\xE2\x80\xA6")) != std::string::npos);
}

TEST(a_limit_of_zero_turns_gives_no_tree_and_not_a_crash) {
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", "задача"));
    h.push_back(assistant_message("msg_000000000002", "msg_000000000001",
                                  {done_call("call_0", "read_file", 0)}));
    /* Предел ноль — это не «показать всех», и не падение: пустое дерево. */
    ASSERT_EQ(timeline::timeline_source(h, 0).turns.size(), size_t(0));
    ASSERT_EQ(timeline::timeline_source(h, 0).omitted_turns, size_t(1));
    const timeline::Timeline tl = of(h, 0);
    ASSERT_EQ(tl.turns.size(), size_t(0));
    ASSERT_EQ(timeline::timeline_head(tl).find("ходов 0") != std::string::npos,
              true);
}

TEST(an_empty_history_is_an_empty_tree) {
    const timeline::Timeline tl = of({}, limits::kMaxTimelineTurns);
    ASSERT_EQ(tl.turns.size(), size_t(0));
    ASSERT_EQ(tl.calls_total, size_t(0));
    ASSERT_EQ(tl.omitted_turns, size_t(0));
}

TEST(the_head_of_the_panel_says_how_much_is_on_it) {
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", "задача"));
    h.push_back(assistant_message("msg_000000000002", "msg_000000000001",
                                  {timed_call("call_0", "read_file", 1500),
                                   timed_call("call_1", "bash", 500)}));
    const timeline::Timeline tl = of(h, limits::kMaxTimelineTurns);
    ASSERT_EQ(timeline::timeline_head(tl),
              std::string("Таймлайн: ходов 1, вызовов 2, время вызовов: 2.0 с"));
}

/* Проверка формы строки на ВСЕХ статусах хода сразу: подписи различаются
 * и человеком, и golden-снапшотом 11.13, а расходятся они тихо. */
TEST(the_states_of_a_turn_are_named_differently) {
    ASSERT_EQ(std::string(timeline::turn_status_label(timeline::TurnStatus::Text)),
              std::string("только текст"));
    ASSERT_EQ(std::string(timeline::turn_status_label(timeline::TurnStatus::Working)),
              std::string("работает"));
    ASSERT_EQ(std::string(timeline::turn_status_label(timeline::TurnStatus::Failed)),
              std::string("с ошибкой"));
    ASSERT_EQ(std::string(timeline::turn_status_label(timeline::TurnStatus::Done)),
              std::string("готов"));
}

/* Признак того, что подписи строк дерева не перепутаны между собой: у
 * каждого вызова своя строка, и строк ровно столько, сколько вызовов. */
TEST(every_call_gets_exactly_one_line_of_its_own) {
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", "задача"));
    h.push_back(assistant_message("msg_000000000002", "msg_000000000001",
                                  {done_call("call_0", "read_file", 1),
                                   done_call("call_0", "read_file", 1),
                                   MessagePart::tool("call_2", "bash")
                                       .set_running()}));
    const timeline::Timeline tl = of(h, limits::kMaxTimelineTurns);
    const std::vector<std::string> labels = call_labels(tl.turns[0]);
    ASSERT_EQ(labels.size(), size_t(3));
    /* Два одинаковых вызова различаются идентификатором: без него окно
     * склеило бы их в одну строку (ImGui-идентификаторы), и дерево показало
     * бы два вызова как один. */
    ASSERT_EQ(tl.turns[0].calls[0].call_id, std::string("call_0"));
    ASSERT_EQ(tl.turns[0].calls[1].call_id, std::string("call_0"));
    ASSERT_EQ(tl.turns[0].calls[2].call_id, std::string("call_2"));
    /* Подпись рабочего вызова отличается от подписи готового: иначе
     * человек, смотрящий на дерево, не отличил бы «работает» от «сделал». */
    ASSERT_EQ(line_with(labels, "read_file: готов, "), labels[0]);
    ASSERT_EQ(line_with(labels, "bash: работает, —"), labels[2]);
    ASSERT_TRUE(labels[0] != labels[2]);
}
/* ======================================================================
 * И11.5: режим показа деталей (showDetails)
 *
 * Предмет проверки — решение из шапки core/timeline.h: скрываются только
 * УСПЕШНЫЕ вызовы, неуспешные показываются целиком, а тело скрытого не
 * собирается вовсе. Окно тут ни при чём (тестового харнесса для ImGui нет),
 * поэтому проверяются две вещи: как вызов показан (call_show) и какие тела
 * кадру нужны (body_call_ids) — вторая впрямую со снимками 11.4, потому
 * что «нужно» без «снимается» было бы обещанием.
 * ====================================================================== */
namespace {

/* Готовый вызов С ТЕКСТОМ ОТВЕТА: тело скрытого вызова не собирается, и
 * без текста проверка «снимка нет» прошла бы на пустом вызове. */
MessagePart ok_call_with_output(const char* call_id, const char* tool,
                                const char* output) {
    MessagePart p = MessagePart::tool(call_id, tool);
    p.set_running();
    ToolOutput out;
    out.title = "read a.txt";
    out.output = output;
    return p.set_result(out);
}

MessagePart failed_call(const char* call_id, const char* tool) {
    MessagePart p = MessagePart::tool(call_id, tool);
    p.set_running();
    return p.set_error("[отказ] пользователь не разрешил");
}

/* Сколько раз идентификатор встречается в списке — счёт САМОЙ проверки, а не
 * вызов production-функции (тот же приём, что у chars_of выше). */
size_t count_of(const std::vector<std::string>& ids, const std::string& id) {
    size_t n = 0;
    for (const std::string& s : ids) {
        if (s == id) ++n;
    }
    return n;
}

/* Ход с одним готовым, одним отказом и одним работающим вызовом: все три
 * состояния, из которых режим выбирает, и оба исхода (скрыт/показан). */
std::vector<Message> mixed_history() {
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", "задача"));
    h.push_back(assistant_message(
        "msg_000000000002", "msg_000000000001",
        {ok_call_with_output("call_ok", "read_file", "содержимое файла"),
         failed_call("call_err", "write_file"),
         MessagePart::tool("call_run", "bash").set_running()}));
    return h;
}

} // namespace

TEST(details_all_leaves_every_call_a_branch) {
    const timeline::Timeline tl = of(mixed_history(), limits::kMaxTimelineTurns);
    const std::vector<timeline::Call>& calls = tl.turns[0].calls;
    ASSERT_EQ(calls.size(), size_t(3));
    /* Ровно по одному утверждению на состояние: «все — ветки» само по себе
     * не сказало бы, какой вывод сломан. */
    for (const timeline::Call& c : calls) {
        ASSERT_TRUE(timeline::call_show(c, timeline::Details::All) ==
                    timeline::CallShow::Branch);
    }
}

TEST(details_only_problems_hides_the_successful_call_and_shows_the_others_whole) {
    const timeline::Timeline tl = of(mixed_history(), limits::kMaxTimelineTurns);
    const std::vector<timeline::Call>& calls = tl.turns[0].calls;
    ASSERT_EQ(calls.size(), size_t(3));
    /* УСПЕШНЫЙ скрыт — это и есть строка задачи. */
    ASSERT_TRUE(timeline::call_show(calls[0], timeline::Details::OnlyProblems) ==
                timeline::CallShow::Row);
    /* Отказ показан ЦЕЛИКОМ: выключатель, прячущий отказ, прятал бы ровно
     * то, ради чего его выключают. */
    ASSERT_TRUE(timeline::call_show(calls[1], timeline::Details::OnlyProblems) ==
                timeline::CallShow::Body);
    /* Работающий — тоже: он ещё движется, и одной строки мало. */
    ASSERT_TRUE(timeline::call_show(calls[2], timeline::Details::OnlyProblems) ==
                timeline::CallShow::Body);
}

TEST(a_hidden_call_stays_in_the_tree_with_its_label) {
    /* Скрытый вызов — ОДНА СТРОКА, а не пустота: он остаётся в дереве и
     * подписывается. Человек отличает спрятанный вызов от несуществовавшего,
     * а по одной строке — что вызов вообще был. */
    const timeline::Timeline tl = of(mixed_history(), limits::kMaxTimelineTurns);
    const std::vector<timeline::Call>& calls = tl.turns[0].calls;
    ASSERT_EQ(calls.size(), size_t(3));
    ASSERT_TRUE((int)calls[0].status == (int)ToolState::Completed);
    ASSERT_EQ(line_with(call_labels(tl.turns[0]), "read_file: готов, "),
              timeline::call_label(calls[0]));
    /* Режим решает ПОКАЗ, а не данные: тот же вызов при включённых деталях
     * тело получает, при выключенных — нет. */
    const std::vector<std::string> open = {"call_ok"};
    const std::vector<std::string> all =
        timeline::body_call_ids(tl, open, timeline::Details::All);
    ASSERT_EQ(all.size(), size_t(1));
    ASSERT_EQ(all[0], std::string("call_ok"));
    const std::vector<std::string> only =
        timeline::body_call_ids(tl, open, timeline::Details::OnlyProblems);
    ASSERT_EQ(only.size(), size_t(2));
    ASSERT_EQ(count_of(only, "call_ok"), size_t(0));
    /* Раскрытая ветка успешного вызова НЕ теряется вместе с режимом: список
     * раскрытых в окне переключатель не трогает, поэтому тело вернётся, как
     * только человек включит детали обратно. */
    ASSERT_EQ(timeline::body_call_ids(tl, open, timeline::Details::All)[0],
              std::string("call_ok"));
}

TEST(a_body_shown_whole_reaches_the_widget_without_being_opened_by_hand) {
    /* Сквозная проверка задачи: неуспешный вызов получает тело, которое до
     * него получали только раскрытые по клику. Человек ничего не нажимал,
     * и в списке раскрытых его идентификатора нет. */
    const std::vector<Message> h = mixed_history();
    const timeline::Timeline tl = timeline::build_timeline(
        timeline::timeline_source(h, limits::kMaxTimelineTurns));
    const std::vector<std::string> ids =
        timeline::body_call_ids(tl, std::vector<std::string>(),
                                timeline::Details::OnlyProblems);
    ASSERT_EQ(ids.size(), size_t(2));
    ASSERT_EQ(ids[0], std::string("call_err"));
    ASSERT_EQ(ids[1], std::string("call_run"));
    const tool_display::Snapshots shots = tool_display::snapshot_open_calls(
        h, ids, limits::kMaxToolDisplayOpen, limits::kMaxToolDisplayChars);
    ASSERT_EQ(shots.views.size(), size_t(2));
    ASSERT_EQ(shots.views[0].call_id, std::string("call_err"));
    ASSERT_TRUE(shots.views[0].error.find("не разрешил") != std::string::npos);
    /* Успешного вызова среди тел нет: его вывод не читается ради строки. */
    for (const tool_display::CallView& v : shots.views) {
        ASSERT_TRUE(v.call_id != "call_ok");
    }
}

TEST(a_hidden_call_is_not_snapshotted_at_all) {
    /* Экономия под чужим мьютексом: вывода скрытого вызова не читаем. Если
     * бы снимок брался, работа шла бы под `state_.mtx` на каждом кадре ради
     * вывода, который никто не увидит (limits.h, И11.4). */
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", "задача"));
    h.push_back(assistant_message(
        "msg_000000000002", "msg_000000000001",
        {ok_call_with_output("call_0", "read_file", "содержимое файла"),
         ok_call_with_output("call_1", "read_file", "ещё содержимое")}));
    const timeline::Timeline tl = of(h, limits::kMaxTimelineTurns);
    const std::vector<std::string> ids =
        timeline::body_call_ids(tl, std::vector<std::string>(),
                                timeline::Details::OnlyProblems);
    ASSERT_TRUE(ids.empty());
    const tool_display::Snapshots shots = tool_display::snapshot_open_calls(
        h, ids, limits::kMaxToolDisplayOpen, limits::kMaxToolDisplayChars);
    ASSERT_EQ(shots.views.size(), size_t(0));
    ASSERT_EQ(shots.omitted, size_t(0));
    /* Снимок этих вызовов СУЩЕСТВУЕТ, то есть проверка не пустая: список
     * пуст не потому, что снимки не снимаются. */
    ASSERT_EQ(tool_display::snapshot_open_calls(
                  h, std::vector<std::string>{"call_0", "call_1"},
                  limits::kMaxToolDisplayOpen, limits::kMaxToolDisplayChars)
                  .views.size(),
              size_t(2));
}

TEST(the_bodies_come_in_the_order_of_the_tree) {
    /* Порядок — порядок ходов И порядок частей внутри хода, а не порядок
     * того, как человек раскрывал ветки: предел применяется к первому
     * попавшему, и при двух местах порядка одно из них оказалось бы «как
     * повезло». Первый ход содержит ДВА проблемных вызова — иначе порядок
     * внутри хода нечем было бы отличить от порядка ходов. */
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", "задача"));
    h.push_back(assistant_message(
        "msg_000000000002", "msg_000000000001",
        {ok_call_with_output("call_a", "read_file", "a"),
         failed_call("call_b1", "write_file"),
         failed_call("call_b2", "edit_file")}));
    h.push_back(assistant_message("msg_000000000003", "msg_000000000001",
                                  {failed_call("call_c", "apply_patch")}));
    const timeline::Timeline tl = of(h, limits::kMaxTimelineTurns);
    const std::vector<std::string> ids = timeline::body_call_ids(
        tl, std::vector<std::string>{"call_c", "call_b2"},
        timeline::Details::OnlyProblems);
    ASSERT_EQ(ids.size(), size_t(3));
    ASSERT_EQ(ids[0], std::string("call_b1"));
    ASSERT_EQ(ids[1], std::string("call_b2"));
    ASSERT_EQ(ids[2], std::string("call_c"));
}

TEST(a_call_shown_whole_is_not_asked_for_twice_when_it_was_already_opened) {
    /* Список тел — по одному элементу на МЕСТО вызова в дереве, повторов
     * идентификатора в нём нет; повтор отсекается там, где он стоит слота
     * предела, — в снимке (`snapshot_open_calls`, И11.5). Одинаковые
     * идентификаторы в истории не выдумка фикстуры: вызов закрывается по
     * идентификатору (И11.3), и пара частей с одним `call_0` встречается в
     * тестах дерева. */
    std::vector<Message> dup;
    dup.push_back(user_message("msg_000000000001", "задача"));
    dup.push_back(assistant_message(
        "msg_000000000002", "msg_000000000001",
        {failed_call("call_0", "write_file"), failed_call("call_0", "edit_file"),
         failed_call("call_1", "bash")}));
    const timeline::Timeline tl = of(dup, limits::kMaxTimelineTurns);
    ASSERT_EQ(tl.turns[0].calls.size(), size_t(3));
    /* Режим «только неуспешные»: два места с одним идентификатором дают два
     * элемента списка, и столько же — при раскрытии по клику. */
    const std::vector<std::string> shown = timeline::body_call_ids(
        tl, std::vector<std::string>(), timeline::Details::OnlyProblems);
    ASSERT_EQ(shown.size(), size_t(3));
    ASSERT_EQ(shown[0], std::string("call_0"));
    ASSERT_EQ(shown[1], std::string("call_0"));
    ASSERT_EQ(shown[2], std::string("call_1"));
    const std::vector<std::string> opened = timeline::body_call_ids(
        tl, std::vector<std::string>{"call_0", "call_1"}, timeline::Details::All);
    ASSERT_EQ(opened.size(), size_t(3));
    /* А СНИМКОВ два, а не три: повторный идентификатор не съедает слот
     * предела и не попадает в пропущенные. */
    const tool_display::Snapshots shots = tool_display::snapshot_open_calls(
        dup, shown, limits::kMaxToolDisplayOpen, limits::kMaxToolDisplayChars);
    ASSERT_EQ(shots.views.size(), size_t(2));
    ASSERT_EQ(shots.views[0].call_id, std::string("call_0"));
    ASSERT_EQ(shots.views[1].call_id, std::string("call_1"));
    ASSERT_EQ(shots.omitted, size_t(0));
}

TEST(the_opened_branches_are_taken_and_nothing_else) {
    /* При включённых деталях список — ровно раскрытые ветки, и чужие
     * идентификаторы в него не попадают. */
    const timeline::Timeline tl = of(mixed_history(), limits::kMaxTimelineTurns);
    const std::vector<std::string> ids = timeline::body_call_ids(
        tl, std::vector<std::string>{"call_run", "call_ok"}, timeline::Details::All);
    ASSERT_EQ(ids.size(), size_t(2));
    ASSERT_EQ(ids[0], std::string("call_ok"));
    ASSERT_EQ(ids[1], std::string("call_run"));
    /* Ничего не раскрыто — нечего и снимать. */
    ASSERT_TRUE(timeline::body_call_ids(tl, std::vector<std::string>(),
                                        timeline::Details::All)
                    .empty());
}

TEST(a_call_outside_the_frame_is_not_asked_for_a_body) {
    /* Идентификатор из хода за пределом kMaxTimelineTurns в список не
     * значит: дерево его не рисует, а снимок читался бы под мьютексом ради
     * ничего. Проверка не пустая — прямой вызов снимка по тому же
     * идентификатору снимок даёт, то есть вызов в истории есть. */
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", "задача"));
    h.push_back(assistant_message(
        "msg_000000000002", "msg_000000000001",
        {failed_call("call_old", "write_file")}));
    h.push_back(assistant_message(
        "msg_000000000003", "msg_000000000002",
        {failed_call("call_new", "edit_file")}));
    const timeline::Timeline tl = of(h, 1);
    ASSERT_EQ(tl.turns.size(), size_t(1));
    ASSERT_EQ(tl.turns[0].calls[0].call_id, std::string("call_new"));
    const std::vector<std::string> open = {"call_old", "call_new"};
    const std::vector<std::string> ids =
        timeline::body_call_ids(tl, open, timeline::Details::OnlyProblems);
    ASSERT_EQ(ids.size(), size_t(1));
    ASSERT_EQ(ids[0], std::string("call_new"));
    /* То же и при включённых деталях: раскрытие «за кадром» тела не даёт.
     * Сама запись в списке раскрытых при этом не теряется — вернётся, когда
     * ход попадёт в кадр. */
    ASSERT_EQ(timeline::body_call_ids(tl, open, timeline::Details::All).size(),
              size_t(1));
    ASSERT_EQ(tool_display::snapshot_open_calls(
                  h, std::vector<std::string>{"call_old"},
                  limits::kMaxToolDisplayOpen, limits::kMaxToolDisplayChars)
                  .views.size(),
              size_t(1));
}

TEST(the_bodies_whole_go_through_the_cap_like_the_opened_ones) {
    /* Режим не обходит предел: иначе десять отказов читались бы под
     * мьютексом агента целиком, и предел И11.4 перестал бы значить то,
     * что написан в limits.h. */
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", "задача"));
    std::vector<MessagePart> parts;
    for (int i = 0; i < 10; ++i) {
        char id[32];
        std::snprintf(id, sizeof(id), "call_%02d", i);
        parts.push_back(failed_call(id, "write_file"));
    }
    h.push_back(assistant_message("msg_000000000002", "msg_000000000001",
                                  std::move(parts)));
    const timeline::Timeline tl = of(h, limits::kMaxTimelineTurns);
    const std::vector<std::string> ids =
        timeline::body_call_ids(tl, std::vector<std::string>(),
                                timeline::Details::OnlyProblems);
    ASSERT_EQ(ids.size(), size_t(10));
    const tool_display::Snapshots shots = tool_display::snapshot_open_calls(
        h, ids, limits::kMaxToolDisplayOpen, limits::kMaxToolDisplayChars);
    ASSERT_EQ(shots.views.size(), limits::kMaxToolDisplayOpen);
    ASSERT_EQ(shots.omitted, size_t(2));
}

TEST(the_details_line_says_what_is_shown_and_how_much_is_hidden) {
    const timeline::Timeline tl = of(mixed_history(), limits::kMaxTimelineTurns);
    ASSERT_EQ(timeline::details_line(tl, timeline::Details::All),
              std::string("детали: тела всех вызовов"));
    /* Число скрытых считается по тому же решению, что рисует окно: здесь
     * один успешный из трёх. */
    ASSERT_EQ(timeline::details_line(tl, timeline::Details::OnlyProblems),
              std::string("детали: тела только неуспешных вызовов; успешных "
                          "скрыто: 1"));
}

TEST(an_empty_history_hides_nothing_and_says_so) {
    /* Ноль скрытых говорится НОЛЕМ, а не отсутствием строки: пустое дерево
     * и дерево, где скрывать нечего, выглядели бы одинаково. */
    const timeline::Timeline tl = of(std::vector<Message>(), limits::kMaxTimelineTurns);
    ASSERT_TRUE(tl.turns.empty());
    ASSERT_TRUE(timeline::body_call_ids(tl, std::vector<std::string>(),
                                        timeline::Details::OnlyProblems)
                    .empty());
    ASSERT_EQ(timeline::details_line(tl, timeline::Details::OnlyProblems),
              std::string("детали: тела только неуспешных вызовов; успешных "
                          "скрыто: 0"));
}
