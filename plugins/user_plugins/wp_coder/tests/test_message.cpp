/*
 * test_message.cpp — И5.4: сессионная модель сообщений.
 *
 * Проверяются три вещи, и все три — про то, что модель увидит в
 * истории, а не про наличие полей:
 *
 *  1. Часть несёт ровно свои данные (таблица «вид → заполненные
 *     аксессоры»). Аксессор, вызванный на чужом виде, обязан быть пуст:
 *     иначе у части-инструмента отросло бы «размышление», и UI
 *     нарисовал бы то, чего агент не говорил.
 *  2. Четыре состояния вызова РАЗЛИЧИМЫ. Прежних двух («есть текст
 *     результата / нет») не хватало: прерванный посреди инструмента ход
 *     выглядел как «инструмента не было», и результат терялся молча.
 *     На этом различии построено условие завершения хода (И5.8).
 *  3. Ход с вызовом инструментов НЕ помещается в одно сообщение, и
 *     to_model_messages обязан его разделить: вызов — реплика
 *     ассистента, результат — реплика пользователя. Склеенное сообщение
 *     сказало бы модели, что результат написала она сама.
 */

#include "test_framework.h"
#include "test_printers.h"
#include "../core/id_prefix.h"
#include "../core/json_utils.h"
#include "../core/message.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace coder;

namespace {

json::JsonValue read_args() {
    json::JsonValue a = json::JsonValue::object();
    a.set("path", "core/engine.cpp");
    a.set("offset", 42);
    return a;
}

/* Один вызов = одна часть, состояние которой меняется на месте. Именно
 * так устроена модель: две части на один вызов означали бы, что одна из
 * них навсегда осталась «не начатой». */
MessagePart running_call(const char* call_id, const char* tool,
                         const json::JsonValue& args, std::string raw,
                         const std::string& result) {
    MessagePart p = MessagePart::tool(call_id, tool, args, std::move(raw));
    p.set_running();
    ToolOutput out;
    out.output = result;
    p.set_result(out);
    return p;
}

MessagePart refused_running_call(const char* call_id, const char* tool,
                                 const json::JsonValue& args, std::string raw,
                                 const std::string& error) {
    MessagePart p = MessagePart::tool(call_id, tool, args, std::move(raw));
    p.set_running();
    p.set_error(error);
    return p;
}

MessagePart finished_call() {
    ToolOutput out;
    out.output = "прочитал 404 строки";
    return MessagePart::tool("call_0", "read_file", read_args())
        .set_result(out);
}

MessagePart refused_call() {
    return MessagePart::tool("call_0", "deploy", read_args())
        .set_error("[отказ] пользователь не разрешил");
}

/* Имена аксессоров, у которых значение непустое. */
std::vector<std::string> filled_accessors(const MessagePart& p) {
    std::vector<std::string> v;
    if (!p.text().empty()) v.push_back("text");
    if (!p.raw_call().empty()) v.push_back("raw_call");
    if (!p.call_id().empty()) v.push_back("call_id");
    if (!p.tool_name().empty()) v.push_back("tool_name");
    if (p.args().type() != json::JsonValue::Type::Null) v.push_back("args");
    if (!p.output().output.empty() || !p.output().title.empty() ||
        !p.output().metadata.is_null() || p.output().truncated) {
        v.push_back("output");
    }
    if (!p.error().empty()) v.push_back("error");
    if (!p.step_name().empty()) v.push_back("step_name");
    if (!p.snapshot_hash().empty()) v.push_back("snapshot_hash");
    if (p.files().type() != json::JsonValue::Type::Null) v.push_back("files");
    if (!p.task_id().empty()) v.push_back("task_id");
    if (!p.subagent().empty()) v.push_back("subagent");
    if (!p.replaced_ids().empty()) v.push_back("replaced_ids");
    if (p.attempt() != 0) v.push_back("attempt");
    if (p.next_attempt_in_ms() != 0) v.push_back("next_attempt_in_ms");
    return v;
}

std::vector<PartKind> all_kinds() {
    return {
        PartKind::Text,      PartKind::Reasoning,   PartKind::Tool,
        PartKind::StepStart, PartKind::StepFinish,  PartKind::Patch,
        PartKind::Retry,     PartKind::Compaction,  PartKind::CompactionContinue,
        PartKind::Subtask,
    };
}

/* По одной части каждого вида, заполненной «по-всей-вашей» — и таблица
 * ожиданий: вид → аксессоры, которые обязаны быть заполнены. */
std::vector<MessagePart> sample_parts() {
    json::JsonValue files = json::JsonValue::array();
    files.push_back(json::JsonValue("core/engine.cpp"));
    return {
        MessagePart::text("итог"),
        MessagePart::reasoning("рассуждение"),
        finished_call(),
        MessagePart::step_start("step_1"),
        MessagePart::step_finish("step_1"),
        MessagePart::patch("a1b2c3", files),
        MessagePart::retry(2, 1500),
        MessagePart::compaction("сводка", {"msg_1", "msg_2"}),
        MessagePart::compaction_continue(),
        MessagePart::subtask("ses_child", "wp_explore"),
    };
}

std::map<std::string, std::vector<std::string>> expected_filled() {
    return {
        {"text",       {"text"}},
        {"reasoning",  {"text"}},
        /* Часть-вызов в таблице без сырого блока: raw_call заполняется
         * только когда блок был (его проверяет отдельный тест). */
        {"tool",       {"call_id", "tool_name", "args", "output"}},
        {"step_start", {"step_name"}},
        {"step_finish",{"step_name"}},
        {"patch",      {"snapshot_hash", "files"}},
        {"retry",      {"attempt", "next_attempt_in_ms"}},
        {"compaction", {"text", "replaced_ids"}},
        {"compaction_continue", {"text"}},
        {"subtask",    {"task_id", "subagent"}},
    };
}

/* Ход ассистента: текст, вызов, результат. Самая частая форма. */
Message tool_turn() {
    Message m;
    m.id = "msg_2";
    m.role = kRoleAssistant;
    m.parent_id = "msg_1";
    m.parts.push_back(MessagePart::text("Смотрю файл."));
    m.parts.push_back(running_call("call_0", "read_file", read_args(),
                                   "{\"tool\":\"read_file\"}",
                                   "engine.h — Универсальный ReAct-движок"));
    return m;
}

} // namespace

/* --- Виды частей --- */

TEST(message_part_kinds_are_named_and_closed) {
    std::set<std::string> names;
    for (PartKind k : all_kinds()) {
        const char* name = part_kind_name(k);
        ASSERT_TRUE(name != nullptr);
        ASSERT_TRUE(std::string(name) != "unknown");
        names.insert(name);
    }
    /* Десять видов: шесть работают, четыре (Patch, Retry, Subtask и
     * запись сжатой истории) объявлены заранее под И10, И12.6, И8 и
     * 7.10 — чтобы файл сессии не пришлось менять по формату при их
     * появлении. */
    ASSERT_EQ(names.size(), size_t(10));
    ASSERT_EQ(all_kinds().size(), size_t(10));
    ASSERT_EQ(std::string(part_kind_name(static_cast<PartKind>(99))),
              std::string("unknown"));
}

TEST(every_part_kind_is_in_the_test_table) {
    /* Список видов в тесте написан руками, а перечисление в switch — в
     * коде, и до И7.7 они разошлись бы молча: новый вид добавлялся в
     * enum, таблица проверок о нём не знала, и «девять видов» оставалось
     * верным числом при десяти. Проверка обходит enum по имени до
     * «unknown» и требует, чтобы каждый вид был в таблице — иначе вид,
     * для которого никто не придумал проверку, просто не проверяется.
     *
     * Имя намеренно кириллицей: тесты этого файла английские, а названия
     * проверок читаются в выводе, где английский «every_part_kind…» рядом
     * с кучей таких же не отличить от прочих. */
    std::set<PartKind> listed;
    for (PartKind k : all_kinds()) listed.insert(k);
    for (int i = 0;; ++i) {
        const PartKind k = static_cast<PartKind>(i);
        if (std::string(part_kind_name(k)) == "unknown") break;
        if (!listed.count(k)) {
            std::cerr << "  вид части " << part_kind_name(k)
                      << " не в таблице all_kinds(): для него нет ни"
                      << " проверки полей, ни round-trip. Добавь его в"
                      << " all_kinds(), sample_parts() и expected_filled()."
                      << std::endl;
        }
        ASSERT_TRUE(listed.count(k) == 1);
    }
    ASSERT_EQ(listed.size(), size_t(10));
}

TEST(an_unfinished_call_hides_its_output_from_everyone) {
    /* Механизм, на котором держится правило «прореживание не трогает
     * незавершённые вызовы» (И7.9): у вызова, который ещё не отработал,
     * вывода НЕТ — не «вывод есть, но его нельзя показывать».
     *
     * Проверка живёт здесь, а не рядом с прореживанием, потому что
     * обеспечено оно здесь: аксессор output() отдаёт пустой результат для
     * любого состояния, кроме completed. Правило держится один раз и для
     * всех вызывающих (UI, сводка, оценка объёма), а не в каждом по
     * своему разу — иначе оно разъехалось бы при первом же новом
     * читателе history.
     *
     * Первая версия проверки жила рядом с прореживанием и строила фикстуру
     * «работающий вызов с непустым output» — такого вызова не существует,
     * set_result кладёт результат и сразу переводит вызов в completed.
     * Фикстура проходила при любом коде, то есть проверяла себя. */
    ToolOutput out;
    out.title = "bash";
    out.output = "вывод на 4000 символов";

    MessagePart running = MessagePart::tool("call_0", "bash").set_result(out);
    running.set_running();
    ASSERT_EQ(std::string(running.state_name()), std::string("running"));
    ASSERT_TRUE(running.output().output.empty());
    /* Работающий вызов — ещё не исход: has_result() ложно, и это тот же
     * признак, на котором стоит условие завершения хода (И5.8). */
    ASSERT_FALSE(running.has_result());

    MessagePart pending = MessagePart::tool("call_1", "bash");
    ASSERT_TRUE(pending.output().output.empty());

    /* Отказ — тоже не вывод, а error(), и путать их нельзя. */
    MessagePart failed = MessagePart::tool("call_2", "bash").set_error("нет");
    ASSERT_TRUE(failed.output().output.empty());
    ASSERT_EQ(failed.error(), std::string("нет"));

    /* Завершённый — единственный, у кого вывод есть. */
    MessagePart done = MessagePart::tool("call_3", "bash").set_result(out);
    ASSERT_EQ(done.output().output, std::string("вывод на 4000 символов"));
}

TEST(the_continuation_kind_is_named_as_in_the_port) {
    /* Имя вида — это тег в файле сессии и в JSON для UI. Оно совпадает с
     * маркером порта (metadata.compaction_continue), и переименование
     * молча отрезало бы от UI и файла сессии всё, что по этому имени
     * ищется: тот же класс, что версия в четырёх файлах (D12). */
    ASSERT_EQ(std::string(part_kind_name(PartKind::CompactionContinue)),
              std::string("compaction_continue"));
}

TEST(message_part_carries_only_its_own_payload) {
    const std::vector<MessagePart> parts = sample_parts();
    const std::map<std::string, std::vector<std::string>> expected =
        expected_filled();
    for (const MessagePart& p : parts) {
        const std::vector<std::string> got = filled_accessors(p);
        const std::vector<std::string>& want =
            expected.at(part_kind_name(p.kind()));
        if (got != want) {
            std::cerr << "  вид " << p.kind_name() << ": заполнено [";
            for (const std::string& s : got) std::cerr << s << " ";
            std::cerr << "], ожидалось [";
            for (const std::string& s : want) std::cerr << s << " ";
            std::cerr << "]" << std::endl;
        }
        ASSERT_TRUE(got == want);
        ASSERT_EQ(p.is(p.kind()), true);
        ASSERT_EQ(std::string(p.kind_name()),
                  std::string(part_kind_name(p.kind())));
    }
}

TEST(message_part_tool_state_changes_ignore_other_kinds) {
    /* Состояние инструмента у текстовой части не существует. Молчаливый
     * no-op здесь был бы опаснее отказа, поэтому он закреплён тестом. */
    MessagePart t = MessagePart::text("итог");
    ToolOutput out;
    out.output = "мусор";
    t.set_running();
    t.set_result(out);
    t.set_error("мусор");
    ASSERT_EQ(t.kind(), PartKind::Text);
    /* Состояние осталось прежним — именно это и отличает no-op от
     * «тихо испорченной части». */
    ASSERT_EQ(t.state(), ToolState::Pending);
    ASSERT_TRUE(t.output().output.empty());
    ASSERT_TRUE(t.error().empty());
    ASSERT_EQ(t.text(), std::string("итог"));
}

/* --- Четыре состояния вызова --- */

TEST(tool_state_four_states_are_distinguishable) {
    std::set<std::string> names;
    const ToolState states[] = {ToolState::Pending, ToolState::Running,
                                ToolState::Completed, ToolState::Error};
    for (ToolState s : states) {
        const char* name = tool_state_name(s);
        ASSERT_TRUE(name != nullptr);
        names.insert(name);
    }
    ASSERT_EQ(names.size(), size_t(4));

    /* И главное различие, которого раньше не было: «не начал» и «начал,
     * но не закончил» — разные состояния, и оба означают открытую
     * часть. */
    MessagePart pending = MessagePart::tool("call_0", "read_file", read_args());
    MessagePart running = MessagePart::tool("call_0", "read_file", read_args())
                              .set_running();
    ASSERT_EQ(pending.state(), ToolState::Pending);
    ASSERT_EQ(running.state(), ToolState::Running);
    ASSERT_TRUE(pending.is_open());
    ASSERT_TRUE(running.is_open());

    const MessagePart done = finished_call();
    ASSERT_EQ(done.state(), ToolState::Completed);
    ASSERT_TRUE(done.has_result());
    ASSERT_TRUE(!done.is_open());

    const MessagePart refused = refused_call();
    ASSERT_EQ(refused.state(), ToolState::Error);
    ASSERT_TRUE(refused.has_result());
    ASSERT_TRUE(!refused.is_open());
}

TEST(tool_part_keeps_exactly_one_outcome) {
    /* Успех, потом ошибка: остаётся ошибка, а чужой вывод из output()
     * убрался — иначе UI нарисовал бы результат успешного вызова как
     * результат текущего. */
    MessagePart p = finished_call();
    ASSERT_EQ(p.output().output, std::string("прочитал 404 строки"));
    p.set_error("сбой");
    ASSERT_EQ(p.state(), ToolState::Error);
    ASSERT_TRUE(p.output().output.empty());
    ASSERT_EQ(p.error(), std::string("сбой"));

    /* И наоборот: после успеха старой ошибки не остаётся. */
    MessagePart q = refused_call();
    q.set_result(ToolOutput());
    ASSERT_EQ(q.state(), ToolState::Completed);
    ASSERT_TRUE(q.error().empty());
}

/* --- Message --- */

TEST(message_text_joins_only_text_parts) {
    Message m = tool_turn();
    m.parts.insert(m.parts.begin(), MessagePart::reasoning("размышление"));
    ASSERT_EQ(m.text(), std::string("Смотрю файл."));
    ASSERT_EQ(m.is_assistant(), true);
    ASSERT_EQ(m.parent_id, std::string("msg_1"));
    /* Вызов закрыт результатом, поэтому незакрытых частей нет — это и
     * есть условие, на котором И5.8 решит, что ход закончен. */
    ASSERT_TRUE(!m.has_open_tool_part());

    /* Два текстовых блока склеиваются абзацем, а не слитно. */
    Message two;
    two.role = kRoleAssistant;
    two.parts.push_back(MessagePart::text("первый"));
    two.parts.push_back(MessagePart::text("второй"));
    ASSERT_EQ(two.text(), std::string("первый\n\nвторой"));
}

TEST(message_reports_an_open_tool_part) {
    Message m;
    m.role = kRoleAssistant;
    m.parts.push_back(MessagePart::tool("call_0", "read_file", read_args()));
    ASSERT_TRUE(m.has_open_tool_part());
    m.parts.back().set_running();
    ASSERT_TRUE(m.has_open_tool_part());
    m.parts.back().set_result(ToolOutput());
    ASSERT_TRUE(!m.has_open_tool_part());

    /* Текстовая часть открытым инструментом не считается. */
    Message text_only;
    text_only.role = kRoleAssistant;
    text_only.parts.push_back(MessagePart::text("итог"));
    ASSERT_TRUE(!text_only.has_open_tool_part());
}

/* --- Сборка истории для модели --- */

TEST(to_model_messages_splits_a_tool_turn_in_two) {
    const std::vector<ModelMessage> msgs = to_model_messages({tool_turn()});
    ASSERT_EQ(msgs.size(), size_t(2));
    /* Реплика ассистента: текст и ЕГО СОБСТВЕННЫЙ вызов. */
    ASSERT_EQ(msgs[0].role, std::string(kRoleAssistant));
    ASSERT_TRUE(msgs[0].content.find("Смотрю файл.") != std::string::npos);
    ASSERT_TRUE(msgs[0].content.find("{\"tool\":\"read_file\"}") !=
                std::string::npos);
    /* Реплика пользователя: результат. Метка та же, что и раньше, — модель
     * о ней знает из системного промпта. */
    ASSERT_EQ(msgs[1].role, std::string(kRoleUser));
    ASSERT_TRUE(msgs[1].content.rfind("RESULT [read_file]:\n", 0) == 0);
    ASSERT_TRUE(msgs[1].content.find("Универсальный ReAct-движок") !=
                std::string::npos);
    /* Результата в реплике ассистента быть не должно: иначе модель
     * решит, что сама его выдумала. */
    ASSERT_TRUE(msgs[0].content.find("RESULT") == std::string::npos);
}

TEST(to_model_messages_leaves_a_call_whose_result_is_pending) {
    Message m;
    m.role = kRoleAssistant;
    m.parts.push_back(MessagePart::text("Начинаю."));
    m.parts.push_back(MessagePart::tool("call_0", "bash", read_args(),
                                        "{\"tool\":\"bash\"}"));
    m.parts.back().set_running();

    const std::vector<ModelMessage> msgs = to_model_messages({m});
    ASSERT_EQ(msgs.size(), size_t(1));
    ASSERT_EQ(msgs[0].role, std::string(kRoleAssistant));
    ASSERT_TRUE(msgs[0].content.find("bash") != std::string::npos);
    ASSERT_TRUE(msgs[0].content.find("RESULT") == std::string::npos);
    /* Фундамент условия завершения И5.8: ход с таким вызовом закрыть
     * нельзя, и модель должна знать, что вызов ещё идёт. */
    ASSERT_TRUE(m.has_open_tool_part());
}

TEST(to_model_messages_keeps_the_raw_block_the_model_wrote) {
    const std::string raw = "```json\n{\"tool\": \"read_file\", \"path\": \"a.txt\"}\n```";
    Message m;
    m.role = kRoleAssistant;
    MessagePart call = MessagePart::tool("call_0", "read_file", read_args(), raw);
    ASSERT_EQ(call.raw_call(), raw);
    /* Сырой блок читается только через raw_call: через text() он
     * недостижим, и поле было бы «записью в никуда». */
    ASSERT_TRUE(call.text().empty());
    m.parts.push_back(std::move(call));
    const std::vector<ModelMessage> msgs = to_model_messages({m});
    ASSERT_EQ(msgs.size(), size_t(1));
    /* Байт в байт: пересборка блока из аргументов дала бы модели другой
     * текст её же вызова. */
    ASSERT_EQ(msgs[0].content, raw);
}

/* Нативный вызов приходит без блока: тогда транскрипт собирается из
 * аргументов. Это тоже текст самой модели, а не новый формат. */
TEST(to_model_messages_synthesizes_a_call_for_a_native_tool_call) {
    Message m;
    m.role = kRoleAssistant;
    m.parts.push_back(MessagePart::tool("call_x", "read_file", read_args()));
    const std::vector<ModelMessage> msgs = to_model_messages({m});
    ASSERT_EQ(msgs.size(), size_t(1));
    json::JsonValue block;
    ASSERT_TRUE(json::JsonValue::parse(msgs[0].content, block));
    ASSERT_EQ(block.get_string("tool"), std::string("read_file"));
    ASSERT_EQ(block.get_string("path"), std::string("core/engine.cpp"));
    ASSERT_EQ(block.get_int("offset"), 42);
}

TEST(to_model_messages_drops_reasoning_and_service_parts) {
    /* Размышление и служебные виды в транскрипт не попадают: у них нет
     * формата, о котором модель знает, а заводить строку без записи в
     * kBaseSystemPrompt — значит повторить D2. */
    Message m;
    m.role = kRoleAssistant;
    m.parts.push_back(MessagePart::reasoning("внутренний текст модели"));
    m.parts.push_back(MessagePart::step_start("step_1"));
    m.parts.push_back(MessagePart::step_finish("step_1"));
    m.parts.push_back(MessagePart::patch("a1b2", json::JsonValue::array()));
    m.parts.push_back(MessagePart::retry(2, 1000));
    m.parts.push_back(MessagePart::subtask("ses_x", "wp_explore"));
    m.parts.push_back(MessagePart::text("итог"));
    const std::vector<ModelMessage> msgs = to_model_messages({m});
    ASSERT_EQ(msgs.size(), size_t(1));
    ASSERT_EQ(msgs[0].content, std::string("итог"));
    /* Пустое сообщение модель не должно получать: половина провайдеров
     * считает его ошибкой протокола. */
    Message only_noise;
    only_noise.role = kRoleAssistant;
    only_noise.parts.push_back(MessagePart::reasoning("размышление"));
    only_noise.parts.push_back(MessagePart::step_start("step_1"));
    ASSERT_TRUE(to_model_messages({only_noise}).empty());
}

TEST(to_model_messages_reports_a_refusal_as_a_result) {
    Message m;
    m.role = kRoleAssistant;
    m.parts.push_back(refused_running_call(
        "call_0", "deploy", read_args(), "{\"tool\":\"deploy\"}",
        "[отказ] пользователь не разрешил"));
    const std::vector<ModelMessage> msgs = to_model_messages({m});
    ASSERT_EQ(msgs.size(), size_t(2));
    ASSERT_EQ(msgs[1].role, std::string(kRoleUser));
    ASSERT_TRUE(msgs[1].content.rfind("RESULT [deploy]:\n", 0) == 0);
    ASSERT_TRUE(msgs[1].content.find("[отказ]") != std::string::npos);
}

TEST(to_model_messages_keeps_history_order_and_parent_id_out_of_it) {
    /* Порядок задаётся массивом, а не parent_id: после компакшна (И7.8)
     * массив перестаёт быть хронологическим, и сортировка по parent_id
     * переставила бы ходы. */
    Message first;
    first.id = "msg_9";
    first.role = kRoleAssistant;
    first.parts.push_back(MessagePart::text("девятый"));
    Message second;
    second.id = "msg_1";
    second.role = kRoleUser;
    second.parent_id = "msg_9";
    second.parts.push_back(MessagePart::text("первый по id"));
    Message third;
    third.id = "msg_5";
    third.role = kRoleAssistant;
    third.parent_id = "msg_1";
    third.parts.push_back(MessagePart::text("пятый"));

    const std::vector<ModelMessage> msgs =
        to_model_messages({first, second, third});
    ASSERT_EQ(msgs.size(), size_t(3));
    ASSERT_EQ(msgs[0].content, std::string("девятый"));
    ASSERT_EQ(msgs[1].content, std::string("первый по id"));
    ASSERT_EQ(msgs[2].content, std::string("пятый"));
    ASSERT_EQ(msgs[0].role, std::string(kRoleAssistant));
    ASSERT_EQ(msgs[1].role, std::string(kRoleUser));
    ASSERT_EQ(msgs[2].role, std::string(kRoleAssistant));
}

TEST(to_model_messages_falls_back_to_user_for_an_unknown_role) {
    /* Роль приходит из файла сессии, и пустая строка не должна дать
     * сообщение без роли: половина провайдеров такое не принимает. */
    Message m;
    m.role = "";
    m.parts.push_back(MessagePart::text("задача"));
    const std::vector<ModelMessage> msgs = to_model_messages({m});
    ASSERT_EQ(msgs.size(), size_t(1));
    ASSERT_EQ(msgs[0].role, std::string(kRoleUser));
}

TEST(to_model_messages_handles_two_calls_in_one_turn) {
    /* Вызов, результат, вызов, результат — четыре реплики, и порядок
     * именно такой. */
    Message m;
    m.role = kRoleAssistant;
    m.parts.push_back(running_call("call_0", "read_file", read_args(),
                                   "{\"tool\":\"read_file\"}",
                                   "содержимое a"));
    m.parts.push_back(running_call("call_1", "glob", read_args(),
                                   "{\"tool\":\"glob\"}",
                                   "список файлов"));

    const std::vector<ModelMessage> msgs = to_model_messages({m});
    ASSERT_EQ(msgs.size(), size_t(4));
    ASSERT_TRUE(msgs[0].content.find("read_file") != std::string::npos);
    ASSERT_TRUE(msgs[1].content.find("содержимое a") != std::string::npos);
    ASSERT_TRUE(msgs[2].content.find("glob") != std::string::npos);
    ASSERT_TRUE(msgs[3].content.find("список файлов") != std::string::npos);
    ASSERT_TRUE(!m.has_open_tool_part());
}

TEST(to_model_messages_splits_text_around_a_result) {
    /* Текст после результата — это отдельная реплика ассистента, а не
     * продолжение реплики пользователя: иначе модель примет свой же
     * вывод за реплику пользователя. */
    Message m;
    m.role = kRoleAssistant;
    m.parts.push_back(MessagePart::text("до вызова"));
    m.parts.push_back(running_call("call_0", "read_file", read_args(),
                                   "{\"tool\":\"read_file\"}", "результат"));
    m.parts.push_back(MessagePart::text("после вызова"));

    const std::vector<ModelMessage> msgs = to_model_messages({m});
    ASSERT_EQ(msgs.size(), size_t(3));
    ASSERT_TRUE(msgs[0].content.find("до вызова") != std::string::npos);
    ASSERT_TRUE(msgs[1].content.find("результат") != std::string::npos);
    ASSERT_EQ(msgs[2].role, std::string(kRoleAssistant));
    ASSERT_EQ(msgs[2].content, std::string("после вызова"));
}

/* ======================================================================
 * И5.5: идентификаторы
 * ====================================================================== */

TEST(ids_are_prefixed_and_padded_to_a_fixed_width) {
    IdSequence seq;
    const std::string m = seq.next_msg();
    const std::string p = seq.next_part();
    const std::string s = seq.next_session();
    ASSERT_TRUE(id_has_prefix(m, kMsgPrefix));
    ASSERT_TRUE(id_has_prefix(p, kPartPrefix));
    ASSERT_TRUE(id_has_prefix(s, kSessionPrefix));
    /* Префиксы разные: иначе в логе не отличить сообщение от части. */
    ASSERT_TRUE(!id_has_prefix(m, kPartPrefix));
    ASSERT_EQ(m.size(), std::strlen(kMsgPrefix) + kIdDigits);
    ASSERT_EQ(m, std::string("msg_000000000001"));
    ASSERT_EQ(p, std::string("prt_000000000002"));
    ASSERT_EQ(s, std::string("ses_000000000003"));
}

/* Главное свойство: сортировка строк совпадает с порядком выдачи.
 * Без дополнения нулями «msg_9» оказывается больше «msg_10», и
 * отсортированная история разъезжается на десятом сообщении. */
TEST(ids_sort_in_the_order_they_were_issued) {
    IdSequence seq;
    std::vector<std::string> issued;
    for (int i = 0; i < 25; ++i) issued.push_back(seq.next_msg());
    /* Специально проверим переход через разряд: 9 → 10. */
    ASSERT_EQ(issued[8], std::string("msg_000000000009"));
    ASSERT_EQ(issued[9], std::string("msg_000000000010"));

    std::vector<std::string> sorted = issued;
    std::sort(sorted.begin(), sorted.end());
    ASSERT_TRUE(sorted == issued);
    for (size_t i = 1; i < sorted.size(); ++i) {
        ASSERT_TRUE(sorted[i - 1] < sorted[i]);
    }
}

TEST(ids_survive_a_loaded_session) {
    /* Счётчик процесса стартует с нуля, а в файле сессии уже есть
     * msg_000000000412 (И5.6). Без observe() первый новый ход получил бы
     * занятый идентификатор, и два сообщения слиплись бы в одно — молча,
     * потому что обе части выглядели бы валидно. */
    IdSequence seq;
    seq.observe("msg_000000000412");
    seq.observe("prt_000000000413");
    const std::string fresh = seq.next_msg();
    ASSERT_EQ(fresh, std::string("msg_000000000414"));
    ASSERT_TRUE(id_number("msg_000000000412") == 412);

    /* Чужой формат счётчик не двигает: иначе мусор в файле сессии
     * сдвинул бы нумерацию на миллион. */
    IdSequence clean;
    clean.observe("не идентификатор");
    clean.observe("msg_1");
    clean.observe("");
    ASSERT_EQ(clean.next_msg(), std::string("msg_000000000001"));
}

/* Честно о границах этой проверки: потокобезопасность счётчика
 * обеспечивается ТИПОМ (std::atomic внутри), а не этим тестом. Тест
 * ловит обычную ошибку «счётчик — просто int», потому что при таком
 * количестве identфикаторов пересечение случается, но гарантировать
 * его нельзя, и «тест, который иногда ловит» не следует выдавать за
 * доказательство. Настоящая защита здесь — сам тип. */
TEST(ids_are_unique_under_concurrent_use) {
    /* Идентификатор может создать и worker-поток агента, и UI-поток при
     * открытии сессии. Пересечение дало бы два сообщения с одним id. */
    IdSequence seq;
    const int kThreads = 8;
    const int kPerThread = 3000;
    std::vector<std::vector<std::string>> got(kThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&seq, &got, t] {
            for (int i = 0; i < kPerThread; ++i) {
                got[t].push_back(seq.next_msg());
            }
        });
    }
    for (std::thread& th : threads) th.join();

    std::vector<std::string> all;
    for (const auto& v : got) {
        for (const std::string& id : v) all.push_back(id);
    }
    ASSERT_EQ(all.size(), size_t(kThreads * kPerThread));
    std::vector<std::string> sorted = all;
    std::sort(sorted.begin(), sorted.end());
    const auto last = std::unique(sorted.begin(), sorted.end());
    ASSERT_EQ(last - sorted.begin(), static_cast<long>(sorted.size()));
}

/* ======================================================================
 * И5.8: условие завершения хода
 *
 * Проверяется ТАБЛИЦЕЙ, а не тремя тестами: условие — это перечисление
 * «почему задача ещё не закрыта», и ценность проверки в том, что перечислены
 * все причины, а не две из них. Одна пропущенная причина выглядит как
 * «цикл просто иногда идёт дальше», и найти её можно только по симптому.
 *
 * Таблица собирается из настоящих объектов (ответ свёрнут из событий,
 * сообщение склеено через turn_to_message), а не из ручных структур: тогда
 * проверка ловит и рассинхрон между свёрткой и вердиктом, а не только
 * опечатку в самой функции.
 * ====================================================================== */

namespace {

/* Ответ с указанной причиной остановки и текстом нужной длины. */
LlmResponse response_with(const std::string& finish, size_t text_len) {
    LlmResponse r;
    LlmResponse::reduce(r, LlmEvent::text_start());
    LlmResponse::reduce(r, LlmEvent::text_delta(std::string(text_len, 'x')));
    LlmResponse::reduce(r, LlmEvent::text_end());
    LlmResponse::reduce(r, LlmEvent::finish(finish));
    return r;
}

/* Ход из ответа: ровно так, как его собирает цикл. */
Message turn_of(const LlmResponse& response, const std::string& parent) {
    return turn_to_message(response, parent);
}

struct VerdictCase {
    const char* what;
    LlmResponse response;
    Message turn;
    std::string last_user_id;
    TurnVerdict expected;
};

}  // namespace

TEST(turn_verdict_tells_every_reason_the_task_is_not_over) {
    const std::string kUser = "msg_000000000001";
    const std::string kLong = std::string(400, 'x');

    /* Ход с вызовом инструмента: даже при длинном тексте и «stop» от хоста
     * задача не закрыта. Проверяется вторая копия finish — ту, что
     * проставляет адаптер (core/llm_source.h). */
    LlmResponse with_call = response_with("tool-calls", 400);
    LlmResponse::reduce(with_call, LlmEvent::tool_call(
        "call_0", "list_skills", json::JsonValue::object()));
    Message call_turn = turn_of(with_call, kUser);
    /* Часть осталась незакрытой — как в момент, когда инструмент ещё
     * работает. */
    ASSERT_TRUE(call_turn.has_open_tool_part());

    std::vector<VerdictCase> cases = {
        {"finish = tool-calls", response_with("tool-calls", 400),
         turn_of(response_with("tool-calls", 400), kUser), kUser,
         TurnVerdict::NeedsTools},
        /* Любая другая причина остановки (length, stop, «eos») задачу не
         * отменяет: закрывать должна причина, а не её отсутствие. */
        {"причина остановки не tool-calls", response_with("length", 400),
         turn_of(response_with("length", 400), kUser), kUser,
         TurnVerdict::Completed},
        {"finish пуст", response_with("", 400),
         turn_of(response_with("", 400), kUser), kUser, TurnVerdict::NeedsTools},
        /* Незакрытый вызов при finish=tool-calls: первым срабатывает
         * условие 1, и это неважно — закрывать нельзя ни так, ни иначе. */
        {"вызов без исхода", response_with("tool-calls", 400), call_turn, kUser,
         TurnVerdict::NeedsTools},
        {"ход отвечает не на последнюю реплику",
         response_with("stop", 400),
         turn_of(response_with("stop", 400), kUser), "msg_000000000009",
         TurnVerdict::NotLastUser},
        {"текст короче порога", response_with("stop", 10),
         turn_of(response_with("stop", 10), kUser), kUser, TurnVerdict::TooShort},
        {"нормальный итог", response_with("stop", 400),
         turn_of(response_with("stop", 400), kUser), kUser, TurnVerdict::Completed},
    };

    for (const VerdictCase& c : cases) {
        const TurnVerdict got = turn_verdict(c.response, c.turn, c.last_user_id);
        if (got != c.expected) {
            std::cerr << "  причина «" << c.what << "»: ждали "
                      << turn_verdict_name(c.expected) << ", получили "
                      << turn_verdict_name(got) << std::endl;
        }
        ASSERT_EQ((int)got, (int)c.expected);
        ASSERT_TRUE(turn_completes_task(c.response, c.turn, c.last_user_id) ==
                    (c.expected == TurnVerdict::Completed));
    }
}

TEST(turn_verdict_sees_an_open_tool_part_even_with_a_final_finish) {
    /* Порядок условий: незакрытая часть важнее «stop». Иначе задача
     * закроется по тексту, потеряв результат инструмента. */
    const std::string kUser = "msg_000000000001";
    LlmResponse r = response_with("stop", 400);
    Message turn = turn_of(r, kUser);
    /* Ровно то состояние, в котором оказывается ход с вызовом: finish
     * приведён адаптером к «tool-calls», но проверим и второй случай —
     * когда причина остановки «stop», а вызов не закрыт. */
    LlmResponse open = response_with("stop", 400);
    LlmResponse::reduce(open, LlmEvent::tool_call(
        "call_0", "list_skills", json::JsonValue::object()));
    Message open_turn = turn_of(open, kUser);
    ASSERT_TRUE(open_turn.has_open_tool_part());
    ASSERT_EQ((int)turn_verdict(open, open_turn, kUser),
              (int)TurnVerdict::ToolStillOpen);

    /* Закрытый вызов тех же данных — оснований продолжать нет. */
    LlmResponse::reduce(open, LlmEvent::tool_result("call_0", ToolOutput()));
    sync_tool_parts(open_turn, open);
    ASSERT_FALSE(open_turn.has_open_tool_part());
    ASSERT_EQ((int)turn_verdict(open, open_turn, kUser),
              (int)TurnVerdict::Completed);
    /* И turn (без вызовов) тоже закрыт — вывод не зависит от лишних
     * частей, а проверка не падает на пустой. */
    ASSERT_EQ((int)turn_verdict(r, turn, kUser), (int)TurnVerdict::Completed);
}

/* ======================================================================
 * И8.11: извлечение результата задачи из хода
 *
 * Проверяется на Message, потому что это чистая функция ответа хода, а
 * инструмент `task` зовёт её (subagent → run_subagent_turn). Четыре
 * случая — четыре разных ответа, и каждый из них в рантайме выглядел бы
 * одинаково, если бы порядок был не тот.
 * ====================================================================== */

TEST(a_turn_text_is_the_answer_even_when_a_tool_failed_beside_it) {
    Message turn;
    turn.role = kRoleAssistant;
    turn.parts.push_back(MessagePart::text("Прочитал, но вывода нет."));

    /* Случай, ради которого порядок «текст, потом остальное» выбран: у хода
     * есть И текст, И упавший вызов. Отдать ошибку вызова — значит
     * выбросить объяснение, которое субагент уже составил. */
    ToolOutput out;
    out.output = "вывод";
    turn.parts.push_back(MessagePart::tool("call_0", "read_file").set_error(
        "[запрещено] инструмент запрещён правилом"));
    turn.parts.push_back(MessagePart::tool("call_1", "bash").set_result(out));

    ASSERT_EQ(turn.task_answer(), std::string("Прочитал, но вывода нет."));
}

TEST(a_turn_without_text_falls_back_to_the_error_of_its_last_call) {
    Message turn;
    turn.role = kRoleAssistant;
    ToolOutput out;
    out.output = "вывод";
    /* Хелпер, который отработал, и потом хелпер, который нет: итог хода —
     * ПОСЛЕДНИЙ вызов, и его ошибка полезнее вывода предыдущего. */
    turn.parts.push_back(MessagePart::tool("call_0", "bash").set_result(out));
    turn.parts.push_back(MessagePart::tool("call_1", "grep_search").set_error(
        "[ошибка] файла нет"));

    ASSERT_EQ(turn.task_answer(), std::string("[ошибка] файла нет"));
}

TEST(a_turn_without_text_falls_back_to_the_output_of_its_last_call) {
    Message turn;
    turn.role = kRoleAssistant;
    ToolOutput out;
    out.output = "структура проекта";
    turn.parts.push_back(MessagePart::tool("call_0", "list").set_result(out));

    ASSERT_EQ(turn.task_answer(), std::string("структура проекта"));
}

TEST(a_turn_with_nothing_to_answer_answers_nothing) {
    /* Не «текст по умолчанию» и не вывод ПЕРВОГО вызова: пустой ответ
     * обязан остаться пустым, иначе вызывающий получит то, чего
     * субагент не говорил. */
    Message turn;
    turn.role = kRoleAssistant;
    turn.parts.push_back(MessagePart::reasoning("размышление без ответа"));
    ASSERT_EQ(turn.task_answer(), std::string());

    /* Незакрытый вызов — не ответ: ход не закончен, и его вывода ещё
     * нет. */
    Message running;
    running.role = kRoleAssistant;
    running.parts.push_back(
        MessagePart::tool("call_0", "bash").set_running());
    ASSERT_EQ(running.task_answer(), std::string());

    /* И незакрытый вызов НЕ ОТМЕНЯЕТ то, что уже есть: у хода, который
     * оборвался на втором вызове, ответом остаётся вывод первого.
     * Проверка отдельная от предыдущей, потому что по построению они
     * различаются только ей: у вызова в состоянии «работает» вывода нет
     * вовсе (И7.9), так что «взять его» молча даёт пустоту — и без этой
     * строки порядок выбора между вызовами не был бы виден. */
    Message interrupted;
    interrupted.role = kRoleAssistant;
    ToolOutput out;
    out.output = "промежуточный";
    interrupted.parts.push_back(
        MessagePart::tool("call_0", "bash").set_result(out));
    interrupted.parts.push_back(
        MessagePart::tool("call_1", "bash").set_running());
    ASSERT_EQ(interrupted.task_answer(), std::string("промежуточный"));
    /* Порядок дел: пустой результат открытого вызова — тоже пустой. */
    ToolOutput none;
    Message pending;
    pending.role = kRoleAssistant;
    pending.parts.push_back(MessagePart::tool("call_1", "bash").set_result(none));
    ASSERT_EQ(pending.task_answer(), std::string());
}

/* ======================================================================
 * И11.3: длительность вызова
 * ======================================================================
 *
 * Проверяется МЕХАНИЗМ, а не то, что «время где-то есть»: до И11.3 в
 * модели не было ни одного поля времени, и «длительность хода» было нечем
 * показать. Источник выбран один (шапка core/timeline.h, отклонение 152) —
 * интервал между двумя переходами состояния, которые у вызова и так есть.
 *
 * Сон здесь НЕ украшение: без реального интервала между set_running и
 * set_result проверка «время посчиталось» была бы зелёной и на коде, где
 * длительность всегда 0. */

namespace {

/* Пауза, после которой длительность обязана быть не меньше. Сон короче
 * запрошенного не бывает, а длиннее — бывает, поэтому проверки только
 * на «не меньше». */
void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

} // namespace

TEST(the_duration_of_a_call_is_measured_between_its_start_and_its_outcome) {
    MessagePart call = MessagePart::tool("call_0", "read_file");
    /* До старта времени нет, и это НЕ ноль: ноль означал бы «отработал
     * мгновенно», а правды здесь не известно. */
    ASSERT_EQ(call.state(), ToolState::Pending);
    ASSERT_FALSE(call.has_duration());
    ASSERT_TRUE(call.duration_ms() < 0);

    call.set_running();
    ASSERT_EQ(call.state(), ToolState::Running);
    /* У работающего вызова времени ещё нет — закрывать его нечем, и
     * «работает, 0 мс» на дереве читалось бы как «отработал мгновенно». */
    ASSERT_TRUE(call.duration_ms() < 0);

    sleep_ms(30);
    call.set_result(ToolOutput());
    ASSERT_EQ(call.state(), ToolState::Completed);
    ASSERT_TRUE(call.has_duration());
    if (call.duration_ms() < 30) {
        std::cerr << "  длительность " << call.duration_ms()
                  << " мс корота интервала 30 мс" << std::endl;
    }
    ASSERT_TRUE(call.duration_ms() >= 30);
}

TEST(a_refused_call_is_timed_too_and_not_only_a_successful_one) {
    /* Отказ — тоже исход вызова. Без времени остались бы ровно те
     * вызовы, о которых человек хочет знать больше всего: тот, который
     * он не разрешил, и тот, которому отказал режим. */
    MessagePart refused = MessagePart::tool("call_1", "bash").set_running();
    sleep_ms(20);
    refused.set_error("[отказ] пользователь не разрешил");
    ASSERT_EQ(refused.state(), ToolState::Error);
    if (refused.duration_ms() < 20) {
        std::cerr << "  у отказавшегося вызова длительность "
                  << refused.duration_ms() << " мс" << std::endl;
    }
    ASSERT_TRUE(refused.duration_ms() >= 20);
}

TEST(the_second_set_running_does_not_move_the_start_of_the_call) {
    /* Ровно то, что делает цикл: set_running перед запуском инструмента,
     * а потом sync_tool_parts закрывает вызов — и по дороге может снова
     * коснуться состояния. Если бы второе касание двигало начало, у части
     * с задержкой между стартом и закрытием длительность съехала бы в
     * ноль, и это молча выглядело бы как «инструмент отработал мгновенно». */
    MessagePart call = MessagePart::tool("call_2", "bash");
    call.set_running();
    sleep_ms(25);
    call.set_running();
    sleep_ms(25);
    call.set_result(ToolOutput());
    if (call.duration_ms() < 50) {
        std::cerr << "  повторный set_running съел начало: длительность "
                << call.duration_ms() << " мс вместо 50+ мс" << std::endl;
    }
    ASSERT_TRUE(call.duration_ms() >= 50);
}

TEST(a_part_that_never_ran_has_no_duration_and_a_text_part_never_gets_one) {
    /* Вызов, закрытый ошибкой ещё в адаптере (мусорный блок вызова),
     * никогда не начинал работу — и времени у него нет. То же у части не
     * того вида: состояние инструмента не может быть у текстовой
     * части. */
    MessagePart broken = MessagePart::tool("call_3", "bash");
    broken.set_error("[ошибка] блок вызова не разобран");
    ASSERT_EQ(broken.state(), ToolState::Error);
    ASSERT_FALSE(broken.has_duration());

    MessagePart text = MessagePart::text("просто текст");
    text.set_running();
    text.set_result(ToolOutput());
    ASSERT_EQ(text.kind(), PartKind::Text);
    ASSERT_FALSE(text.has_duration());
}

TEST(the_second_outcome_does_not_measure_the_call_again) {
    /* Ровно то, что делает цикл с ходом из ДВУХ вызовов: синхронизация
     * закрывает все вызовы ответа, поэтому после второго инструмента
     * первый закрывается ВТОРЫМ разом. Без защиты «посчитать один раз» его
     * длительность выросла бы на время второго вызова, и дерево показало
     * бы первому вызову время, которого он не занимал. */
    MessagePart first = MessagePart::tool("call_0", "read_file").set_running();
    sleep_ms(20);
    first.set_result(ToolOutput());
    const long long after_first = first.duration_ms();
    ASSERT_TRUE(after_first >= 20);
    /* Второй вызов отработал, синхронизация прошла по обоим. */
    MessagePart second = MessagePart::tool("call_1", "bash").set_running();
    sleep_ms(20);
    second.set_result(ToolOutput());
    first.set_result(ToolOutput());
    if (first.duration_ms() != after_first) {
        std::cerr << "  повторный исход переписал длительность: было "
                  << after_first << ", стало " << first.duration_ms()
                  << " мс" << std::endl;
    }
    ASSERT_EQ(first.duration_ms(), after_first);
}

TEST(a_duration_that_came_from_the_file_is_a_value_and_not_a_measurement) {
    /* Разбор файла сессии — единственный законный путь восстановить
     * длительность: процесса, который её мерил, после перезагрузки уже
     * нет. Значение поэтому просто кладётся, а не вычисляется заново. */
    MessagePart restored = MessagePart::tool("call_4", "read_file");
    restored.restore_duration_ms(1500);
    ASSERT_TRUE(restored.has_duration());
    ASSERT_EQ(restored.duration_ms(), 1500LL);
    ASSERT_EQ((int)restored.state(), (int)ToolState::Pending);

    /* Отрицательное значение — не длительность: правдоподобная цифра из
     * битой строки не должна становиться временем вызова. */
    MessagePart bad = MessagePart::tool("call_5", "read_file");
    bad.restore_duration_ms(-7);
    ASSERT_FALSE(bad.has_duration());
}
