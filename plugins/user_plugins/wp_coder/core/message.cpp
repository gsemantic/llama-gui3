// message.cpp — сессионная модель сообщений (И5.4), работа с историей (И5.7).

#include "message.h"
#include "json_utils.h"
#include "limits.h"
#include "prompts.h"   /* kCompactionContinueText: единственный источник текста
                       * реплики автопродолжения (И7.7) */

namespace coder {
namespace {

const std::string& empty_string() {
    static const std::string kEmpty;
    return kEmpty;
}

const json::JsonValue& empty_json() {
    static const json::JsonValue kEmpty;
    return kEmpty;
}

const std::vector<std::string>& empty_ids() {
    static const std::vector<std::string> kEmpty;
    return kEmpty;
}

const ToolOutput& empty_output() {
    static const ToolOutput kEmpty;
    return kEmpty;
}

bool is_tool_part(const MessagePart& p) {
    return p.kind() == PartKind::Tool;
}

/* Результат или отказ: то, что уходит модели отдельной репликой. */
bool is_tool_outcome(const MessagePart& p) {
    return is_tool_part(p) && p.has_result();
}

} // namespace

const char* tool_state_name(ToolState s) {
    switch (s) {
        case ToolState::Pending:   return "pending";
        case ToolState::Running:   return "running";
        case ToolState::Completed: return "completed";
        case ToolState::Error:     return "error";
    }
    return "?";
}

const char* part_kind_name(PartKind k) {
    switch (k) {
        case PartKind::Text:       return "text";
        case PartKind::Reasoning:  return "reasoning";
        case PartKind::Tool:       return "tool";
        case PartKind::StepStart:  return "step_start";
        case PartKind::StepFinish: return "step_finish";
        case PartKind::Patch:      return "patch";
        case PartKind::Retry:      return "retry";
        case PartKind::Compaction: return "compaction";
        /* Имя вида — это тег в файле сессии и в JSON для UI (И7.7).
         * Оно совпадает с маркером порта (metadata.compaction_continue),
         * и менять его молча нельзя: сессия, записанная старой версией,
         * перестанет читаться, а читается она этим же кодом. */
        case PartKind::CompactionContinue: return "compaction_continue";
        case PartKind::Subtask:    return "subtask";
    }
    return "unknown";
}

/* --- Фабрики --- */

MessagePart MessagePart::text(std::string text) {
    MessagePart p(PartKind::Text);
    p.text_ = std::move(text);
    return p;
}

MessagePart MessagePart::reasoning(std::string text) {
    MessagePart p(PartKind::Reasoning);
    p.text_ = std::move(text);
    return p;
}

MessagePart MessagePart::tool(std::string call_id, std::string tool_name,
                              json::JsonValue args) {
    return tool(std::move(call_id), std::move(tool_name), std::move(args),
                std::string());
}

MessagePart MessagePart::tool(std::string call_id, std::string tool_name,
                              json::JsonValue args, std::string raw_block) {
    MessagePart p(PartKind::Tool);
    p.call_id_ = std::move(call_id);
    p.tool_name_ = std::move(tool_name);
    p.args_ = std::move(args);
    /* Сырой блок вызова хранится рядом с разобранными аргументами.
     * В транскрипт уходит ИМЕННО он, а не пересобранный из аргументов:
     * модель должна увидеть тот же блок, который сама и написала. Иначе
     * на следующем шаге она получит другой текст своего же вызова и не
     * сможет соотнести его с тем, что собиралась сделать. Пустой блок —
     * обычное дело при нативном вызове: тогда транскрипт собирается из
     * аргументов, и это всё равно текст самой модели. */
    p.text_ = std::move(raw_block);
    return p;
}

MessagePart MessagePart::step_start(std::string step_name) {
    MessagePart p(PartKind::StepStart);
    p.step_name_ = std::move(step_name);
    return p;
}

MessagePart MessagePart::step_finish(std::string step_name) {
    MessagePart p(PartKind::StepFinish);
    p.step_name_ = std::move(step_name);
    return p;
}

MessagePart MessagePart::patch(std::string snapshot_hash,
                               json::JsonValue files) {
    MessagePart p(PartKind::Patch);
    p.snapshot_hash_ = std::move(snapshot_hash);
    p.files_ = std::move(files);
    return p;
}

MessagePart MessagePart::retry(int attempt, int next_attempt_in_ms) {
    MessagePart p(PartKind::Retry);
    p.attempt_ = attempt;
    p.next_attempt_in_ms_ = next_attempt_in_ms;
    return p;
}

MessagePart MessagePart::compaction(std::string summary,
                                    std::vector<std::string> replaced_ids) {
    MessagePart p(PartKind::Compaction);
    p.text_ = std::move(summary);
    p.replaced_ids_ = std::move(replaced_ids);
    return p;
}

MessagePart MessagePart::compaction_continue() {
    MessagePart p(PartKind::CompactionContinue);
    p.text_ = kCompactionContinueText;
    return p;
}

MessagePart MessagePart::subtask(std::string task_id, std::string subagent) {
    MessagePart p(PartKind::Subtask);
    p.task_id_ = std::move(task_id);
    p.subagent_ = std::move(subagent);
    return p;
}

/* --- Аксессоры --- */

const std::string& MessagePart::text() const {
    if (kind_ != PartKind::Text && kind_ != PartKind::Reasoning &&
        kind_ != PartKind::Compaction &&
        kind_ != PartKind::CompactionContinue) {
        return empty_string();
    }
    return text_;
}

const std::string& MessagePart::call_id() const {
    if (!is_tool_part(*this)) return empty_string();
    return call_id_;
}

const std::string& MessagePart::tool_name() const {
    if (!is_tool_part(*this)) return empty_string();
    return tool_name_;
}

const json::JsonValue& MessagePart::args() const {
    if (!is_tool_part(*this)) return empty_json();
    return args_;
}

const std::string& MessagePart::raw_call() const {
    if (!is_tool_part(*this)) return empty_string();
    return text_;
}

const ToolOutput& MessagePart::output() const {
    if (!is_tool_part(*this) || state_ != ToolState::Completed) {
        return empty_output();
    }
    return output_;
}

const std::string& MessagePart::error() const {
    if (!is_tool_part(*this) || state_ != ToolState::Error) {
        return empty_string();
    }
    return error_;
}

bool MessagePart::has_result() const {
    return is_tool_part(*this) &&
           (state_ == ToolState::Completed || state_ == ToolState::Error);
}

const std::string& MessagePart::step_name() const {
    if (kind_ != PartKind::StepStart && kind_ != PartKind::StepFinish) {
        return empty_string();
    }
    return step_name_;
}

const std::string& MessagePart::snapshot_hash() const {
    if (kind_ != PartKind::Patch) return empty_string();
    return snapshot_hash_;
}

const json::JsonValue& MessagePart::files() const {
    if (kind_ != PartKind::Patch) return empty_json();
    return files_;
}

const std::string& MessagePart::task_id() const {
    if (kind_ != PartKind::Subtask) return empty_string();
    return task_id_;
}

const std::string& MessagePart::subagent() const {
    if (kind_ != PartKind::Subtask) return empty_string();
    return subagent_;
}

const std::vector<std::string>& MessagePart::replaced_ids() const {
    if (kind_ != PartKind::Compaction) return empty_ids();
    return replaced_ids_;
}

/* --- Переходы состояния вызова --- */

MessagePart& MessagePart::set_running() {
    if (is_tool_part(*this)) state_ = ToolState::Running;
    return *this;
}

MessagePart& MessagePart::set_result(ToolOutput out) {
    if (!is_tool_part(*this)) return *this;
    output_ = std::move(out);
    error_.clear();
    state_ = ToolState::Completed;
    return *this;
}

MessagePart& MessagePart::clear_output() {
    /* Только на ЗАВЕРШЁННОМ вызове: у работающего инструмента вывода ещё
     * нет, а у отказавшего он и так короткий, и метка «очищено» на отказе
     * сбила бы с толку. Возврат ссылки — как у соседних переходов. */
    if (!is_tool_part(*this) || state_ != ToolState::Completed) return *this;
    output_cleared_ = true;
    return *this;
}

MessagePart& MessagePart::set_error(std::string error) {
    if (!is_tool_part(*this)) return *this;
    error_ = std::move(error);
    /* Прежний результат сбрасывается: у вызова может быть ровно один
     * исход, и оставшийся в output() вывод успешного вызова выглядел бы
     * как результат текущего. */
    output_ = ToolOutput();
    state_ = ToolState::Error;
    return *this;
}

/* --- Message --- */

Message Message::user(std::string text) {
    Message m;
    m.id = ids().next_msg();
    m.role = kRoleUser;
    if (!text.empty()) m.parts.push_back(MessagePart::text(std::move(text)));
    return m;
}

Message Message::assistant(std::string parent_id) {
    Message m;
    m.id = ids().next_msg();
    m.role = kRoleAssistant;
    m.parent_id = std::move(parent_id);
    return m;
}

Message Message::compaction_continue() {
    Message m = Message::user(std::string());
    m.parts.push_back(MessagePart::compaction_continue());
    return m;
}

bool Message::has_open_tool_part() const {
    for (const MessagePart& p : parts) {
        if (p.is_open()) return true;
    }
    return false;
}

std::string Message::text() const {
    std::string out;
    for (const MessagePart& p : parts) {
        if (p.kind() != PartKind::Text) continue;
        if (!out.empty()) out += "\n\n";
        out += p.text();
    }
    return out;
}

std::string Message::to_model_string() const {
    const std::vector<ModelMessage> msgs = to_model_messages({*this});
    std::string out;
    for (const ModelMessage& m : msgs) {
        if (!out.empty()) out += "\n\n";
        out += m.content;
    }
    return out;
}

/* ======================================================================
 * Сборка истории для модели
 * ====================================================================== */

namespace {

/* Текст части для транскрипта. Пустая строка означает «в транскрипт не
 * попадает» (размышление, служебные виды) — см. правила в message.h.
 *
 * Часть-инструмент сюда не попадает: она даёт сразу два куска в разные
 * реплики (см. tool_call_text / tool_result_text). */
std::string part_to_model_text(const MessagePart& p) {
    if (p.kind() == PartKind::Text) return p.text();
    /* Реплика автопродолжения — инструкция, а не пометка (И7.7), поэтому
     * она УХОДИТ модели вопреки правилу «служебные виды молчат». Если бы
     * она молчала, агент после сжатия останавливался бы на сводке, и
     * выглядело бы это как «сжатие прошло, задача закрыта». */
    if (p.kind() == PartKind::CompactionContinue) return p.text();
    return std::string();
}

/* Реплика пользователя: результат или отказ. Формат тот же, что был до
 * И5 («RESULT [инструмент]:»), и модель о нём знает из системного
 * промпта — новая строка формата не заводится.
 *
 * Очищенный прореживанием результат (И7.9) отдаётся одной меткой вместо
 * текста: метка названа в kBaseSystemPrompt, и именно поэтому модель знает,
 * что текст убран из-за объёма и его надо получить повторным вызовом, а не
 * выдумать. Само свойство — на части, а не здесь: строка рендерится зря,
 * если часть не очищена, и наоборот. */
std::string tool_result_text(const MessagePart& p) {
    const std::string body = p.output_cleared()
        ? limits::kClearedToolOutput
        : (p.state() == ToolState::Error ? p.error() : p.output().output);
    return "RESULT [" + p.tool_name() + "]:\n" + body;
}

} // namespace

/* Реплика ассистента: сам вызов. Вне анонимного пространства — им
 * пользуется и сводка компакшна (И7.4), и второй сборщик означал бы
 * второй протокол вызова (см. объявление в message.h). */
std::string tool_call_text(const MessagePart& p) {
    if (!p.raw_call().empty()) return p.raw_call();
    if (p.tool_name().empty()) return std::string();
    /* Нативный вызов приходит без блока: собираем JSON из аргументов. Это
     * тоже текст самой модели, а не новый формат. */
    json::JsonValue block = p.args();
    if (!block.is_object()) block = json::JsonValue::object();
    block.set("tool", p.tool_name());
    return block.dump();
}

std::vector<ModelMessage> to_model_messages(const std::vector<Message>& history) {
    std::vector<ModelMessage> out;
    for (const Message& m : history) {
        std::string pending;      /* накопитель текущей реплики */
        const std::string role =
            m.role.empty() ? std::string(kRoleUser) : m.role;

        const auto flush = [&](void) {
            if (pending.empty()) return;
            out.push_back({role, pending});
            pending.clear();
        };
        const auto append = [&](const std::string& t) {
            if (t.empty()) return;
            if (!pending.empty()) pending += "\n\n";
            pending += t;
        };

        for (const MessagePart& p : m.parts) {
            if (is_tool_part(p)) {
                /* Вызов — реплика ассистента, исход — реплика
                 * пользователя. Одна часть хранит и то, и другое, потому
                 * что вызов у неё один; разнести их по репликам обязано
                 * здесь, иначе модель либо не увидит, что вызывала, либо
                 * решит, что результат написала сама. */
                append(tool_call_text(p));
                if (is_tool_outcome(p)) {
                    flush();
                    out.push_back({std::string(kRoleUser),
                                   tool_result_text(p)});
                }
                continue;
            }
            if (p.is(PartKind::Compaction)) {
                /* Сводка лежит в ПОЛЬЗОВАТЕЛЬСКОМ сообщении (так её
                 * создаёт сжатие, И7.8), но по происхождению это ответ
                 * агента-сводщика. Поэтому в транскрипте она становится
                 * отдельной репликой ассистента, а не куском
                 * пользовательской.
                 *
                 * Репликой, а не склейкой: склеенная со сводкой
                 * пользовательская реплика означала бы «человек попросил
                 * составить сводку» — а на такое модель отвечает не
                 * продолжением работы, а новым заданием, то есть агент
                 * после сжатия начал бы СНОВА. Роль здесь не украшение:
                 * она сообщает, кому принадлежит написанное.
                 *
                 * Пустой текст не отправляется (тот же протокольный
                 * страх, что у пустых кусков в message.h): сообщение без
                 * содержимого половина провайдеров считает ошибкой. */
                flush();
                if (!p.text().empty()) {
                    out.push_back({std::string(kRoleAssistant), p.text()});
                }
                continue;
            }
            append(part_to_model_text(p));
        }
        flush();
    }
    return out;
}

/* ======================================================================
 * Работа с историей (И5.7)
 * ====================================================================== */

const Message* find_message(const std::vector<Message>& history,
                            const std::string& id) {
    if (id.empty()) return nullptr;
    for (const Message& m : history) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

Message* find_message(std::vector<Message>& history, const std::string& id) {
    /* const_cast здесь — единственное место во всём ядре, где один и тот же
     * алгоритм отдаёт и «прочитать», и «изменить». Разыменовывать
     * не-константный указатель константной функции нельзя, а второй
     * обход того же кода — это два места, где придётся вспомнить про
     * сравнение идентификаторов. */
    return const_cast<Message*>(find_message(
        static_cast<const std::vector<Message>&>(history), id));
}

const MessagePart* find_tool_part(const Message& msg,
                                  const std::string& call_id) {
    if (call_id.empty()) return nullptr;
    for (const MessagePart& p : msg.parts) {
        if (p.kind() != PartKind::Tool) continue;
        if (p.call_id() == call_id) return &p;
    }
    return nullptr;
}

MessagePart* find_tool_part(Message& msg, const std::string& call_id) {
    return const_cast<MessagePart*>(find_tool_part(
        static_cast<const Message&>(msg), call_id));
}

const Message* last_user_message(const std::vector<Message>& history) {
    for (size_t i = history.size(); i > 0; --i) {
        if (history[i - 1].is_user()) return &history[i - 1];
    }
    return nullptr;
}

Message turn_to_message(const LlmResponse& response,
                        const std::string& parent_id) {
    Message m = Message::assistant(parent_id);
    if (!response.reasoning().empty()) {
        m.parts.push_back(MessagePart::reasoning(response.reasoning()));
    }
    if (!response.text().empty()) {
        m.parts.push_back(MessagePart::text(response.text()));
    }
    for (const LlmToolCall& c : response.tool_calls()) {
        MessagePart part = MessagePart::tool(c.call_id, c.name, c.arguments,
                                             c.raw_input);
        /* Вызов, который провайдер закрыл ошибкой (мусор в аргументах,
         * аргументы без имени инструмента), остаётся закрытым: ждать
         * результата, которого не будет, нельзя — иначе условие
         * завершения (И5.8) не наступит никогда, и ход будет висеть
         * открытым до конца сессии. */
        if (c.finished && c.failed) {
            part.set_error(c.error);
        } else if (c.finished) {
            part.set_result(c.output);
        }
        m.parts.push_back(std::move(part));
    }
    return m;
}

void sync_tool_parts(Message& msg, const LlmResponse& response) {
    for (const LlmToolCall& c : response.tool_calls()) {
        MessagePart* part = find_tool_part(msg, c.call_id);
        /* Вызова нет в сообщении — закрывать нечего. Молчаливый выход
         * здесь опасен только в одну сторону: вызов без части остался бы
         * без исхода и висел бы открытым, поэтому это проверяется тестом
         * на отсутствие такой ситуации, а не «сеттером на всякий случай». */
        if (!part) continue;
        if (c.finished && c.failed) {
            part->set_error(c.error);
        } else if (c.finished) {
            part->set_result(c.output);
        } else if (part->state() == ToolState::Pending) {
            part->set_running();
        }
    }
}

const char* turn_verdict_name(TurnVerdict v) {
    switch (v) {
        case TurnVerdict::Completed:     return "completed";
        case TurnVerdict::NeedsTools:    return "needs_tools";
        case TurnVerdict::ToolStillOpen: return "tool_still_open";
        case TurnVerdict::NotLastUser:   return "not_last_user";
        case TurnVerdict::TooShort:      return "too_short";
    }
    return "unknown";
}

TurnVerdict turn_verdict(const LlmResponse& response, const Message& turn,
                         const std::string& last_user_id) {
    /* 1. Провайдер остановился, чтобы вызвать инструмент, — ход не финальный.
     * «unknown» и пустая причина — в том же списке: неизвестная причина
     * остановки не доказывает завершения, и закрывать по ней задачу —
     * значит потерять остаток ответа. */
    const std::string& finish = response.finish_reason();
    if (finish.empty() || finish == "tool-calls" || finish == "unknown") {
        return TurnVerdict::NeedsTools;
    }
    /* 2. Незакрытая часть-вызов: результата ещё нет, и он был бы потерян. */
    if (turn.has_open_tool_part()) return TurnVerdict::ToolStillOpen;
    /* 3. Ход отвечает на не самое свежее сообщение пользователя: значит, пока
     * он шёл, человек написал ещё одно, и закрывать нечего. */
    if (turn.parent_id != last_user_id) return TurnVerdict::NotLastUser;
    /* 4. Слишком короткий текст — это «начало работы», а не итог. */
    if (response.text().size() < limits::kMinFinalAnswerLen) {
        return TurnVerdict::TooShort;
    }
    return TurnVerdict::Completed;
}

bool turn_completes_task(const LlmResponse& response, const Message& turn,
                         const std::string& last_user_id) {
    return turn_verdict(response, turn, last_user_id) == TurnVerdict::Completed;
}

size_t model_history_chars(const std::vector<Message>& history) {
    size_t total = 0;
    for (const ModelMessage& m : to_model_messages(history)) {
        total += m.content.size();
    }
    return total;
}

namespace {

/* Обрезок по границе UTF-8: ровно до многобайтового символа резать
 * нельзя — в истории появился бы битый байт, который молча ломает и
 * разбор файла сессии, и вывод в UI. */
std::string cut(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    std::string head = text::utf8_prefix(s, max_bytes);
    head += "…";
    return head;
}

/* Первая непустая строка результата — то, по чему модель решает, нужен
 * ли ей результат целиком. */
std::string first_line(const std::string& s, size_t max_bytes) {
    size_t start = 0;
    while (start < s.size()) {
        const size_t nl = s.find('\n', start);
        const std::string line =
            s.substr(start, nl == std::string::npos ? std::string::npos
                                                     : nl - start);
        if (!line.empty()) return cut(line, max_bytes);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return "(без вывода)";
}

const char* kReasoningStub = "(размышление сжато)";

/* Сжать одну часть по правилам compress_history. */
void compress_part(MessagePart& p) {
    if (p.kind() == PartKind::Reasoning) {
        /* Размышление в транскрипт не попадает, поэтому это самый дешёвый
         * способ освободить место: смысл для модели не теряется. */
        p = MessagePart::reasoning(kReasoningStub);
        return;
    }
    if (p.kind() == PartKind::Text) {
        p = MessagePart::text(first_line(p.text(), 60));
        return;
    }
    if (!is_tool_part(p)) return;
    /* САМ вызов не трогаем: стереть его — значит оставить его результат
     * сиротой («RESULT [read_file]» без вызова, который его вызвал). */
    if (p.state() == ToolState::Completed) {
        ToolOutput stub;
        stub.title = p.output().title;
        stub.output = "[сжат] " + first_line(p.output().output, 60);
        p.set_result(stub);
    } else if (p.state() == ToolState::Error) {
        const std::string err = cut(p.error(), 120);
        if (err != p.error()) p.set_error(err);
    }
}

} // namespace

void compress_history(std::vector<Message>& history, size_t budget) {
    size_t total = model_history_chars(history);
    if (total <= budget) return;
    const size_t n = history.size();
    /* Первую задачу и последний ход не трогаем: сжать задачу — значит
     * отнять у модели её цель, а сжать ход, который обсуждается прямо
     * сейчас, — значит стереть рассуждение, к которому модель сейчас
     * возвращается. */
    for (size_t i = 1; i + 1 < n && total > budget; ++i) {
        Message& m = history[i];
        const size_t before = model_history_chars({m});
        for (MessagePart& p : m.parts) compress_part(p);
        const size_t after = model_history_chars({m});
        if (after < before) total -= (before - after);
    }
}

} // namespace coder
