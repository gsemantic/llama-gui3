// message.cpp — сессионная модель сообщений (И5.4).

#include "message.h"

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

MessagePart MessagePart::subtask(std::string task_id, std::string subagent) {
    MessagePart p(PartKind::Subtask);
    p.task_id_ = std::move(task_id);
    p.subagent_ = std::move(subagent);
    return p;
}

/* --- Аксессоры --- */

const std::string& MessagePart::text() const {
    if (kind_ != PartKind::Text && kind_ != PartKind::Reasoning &&
        kind_ != PartKind::Compaction) {
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
    return std::string();
}

/* Реплика ассистента: сам вызов. */
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

/* Реплика пользователя: результат или отказ. Формат тот же, что был до
 * И5 («RESULT [инструмент]:»), и модель о нём знает из системного
 * промпта — новая строка формата не заводится. */
std::string tool_result_text(const MessagePart& p) {
    const std::string body = p.state() == ToolState::Error
        ? p.error()
        : p.output().output;
    return "RESULT [" + p.tool_name() + "]:\n" + body;
}

} // namespace

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
            append(part_to_model_text(p));
        }
        flush();
    }
    return out;
}

} // namespace coder
