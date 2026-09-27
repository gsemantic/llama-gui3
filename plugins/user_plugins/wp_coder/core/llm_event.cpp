// llm_event.cpp — событийная модель ответа LLM (И5.1), учёт токенов (И5.2).

#include "llm_event.h"
#include "json_utils.h"

#include <algorithm>

namespace coder {
namespace {

/* Пустые значения для аксессоров, вызванных не на своём виде события.
 * Функции, а не static внутри аксессоров: адреса должны совпадать, иначе
 * «пусто» окажется разными объектами в разных вызовах. */
const std::string& empty_string() {
    static const std::string kEmpty;
    return kEmpty;
}

const json::JsonValue& empty_json() {
    static const json::JsonValue kEmpty;
    return kEmpty;
}

const Usage& empty_usage() {
    static const Usage kEmpty;
    return kEmpty;
}

/* Провайдер, сообщивший отрицательное число токенов, — это мусор в
 * ответе, а не «минус токенов». Подрезаем, иначе дальше по арифметике
 * едет всё: доля контекста станет отрицательной, а выборка для
 * компакшна (И7) — неправильной. */
long long non_negative(long long v) {
    return v < 0 ? 0 : v;
}

/* Длина символа по первому байту, 0 — байт не может начать
 * последовательность (text::utf8_sequence_at такому байту отказывает,
 * и ждать следующую дельту бессмысленно). */
size_t utf8_expected_len(unsigned char first) {
    if (first <= 0x7F) return 1;
    if (first >= 0xC2 && first <= 0xDF) return 2;
    if (first >= 0xE0 && first <= 0xEF) return 3;
    if (first >= 0xF0 && first <= 0xF4) return 4;
    return 0;
}

} // namespace

/* ======================================================================
 * Usage (И5.2)
 * ====================================================================== */

long long Usage::non_cached_input() const {
    const long long rest = input - cache_read - cache_write;
    return rest < 0 ? 0 : rest;
}

double Usage::cache_ratio() const {
    if (input <= 0) return 0.0;
    return static_cast<double>(cache_read + cache_write) /
           static_cast<double>(input);
}

bool Usage::empty() const {
    return total == 0 && input == 0 && output == 0;
}

bool Usage::invariants_hold() const {
    if (input < 0 || output < 0 || reasoning < 0 ||
        cache_read < 0 || cache_write < 0 || total < 0) return false;
    if (reasoning > output) return false;
    if (total != input + output) return false;
    /* Ключевой инвариант: счётчики не перекрываются. Считаем именно так,
     * а не сравниваем с total — иначе счётчик, испорченный вдвое, дал бы
     * «нулевую» разницу и проверка прошла бы. */
    if (non_cached_input() + cache_read + cache_write != input) return false;
    return true;
}

Usage Usage::from_provider(long long prompt_tokens, long long completion_tokens,
                           long long cache_read_tokens, long long cache_write_tokens,
                           bool cache_counted_in_prompt) {
    Usage u;
    u.output = non_negative(completion_tokens);
    const long long reported = non_negative(prompt_tokens);
    const long long cr = non_negative(cache_read_tokens);
    const long long cw = non_negative(cache_write_tokens);
    if (cache_counted_in_prompt) {
        /* Кэш уже внутри prompt_tokens: повторно его складывать нельзя,
         * иначе input удвоится. */
        u.input = reported;
        /* Битый провайдер может отдать кэша больше, чем входа; тогда
         * непокрытая часть input просто обнуляется, и инвариант
         * остаётся верным вместо отрицательного остатка. */
        u.cache_read = std::min(cr, u.input);
        u.cache_write = std::min(cw, u.input - u.cache_read);
    } else {
        /* Anthropic-стиль: input_tokens — это ТОЛЬКО некэшированная
         * часть, кэш приезжает отдельными числами. */
        u.cache_read = cr;
        u.cache_write = cw;
        u.input = reported + cr + cw;
    }
    u.total = u.input + u.output;
    return u;
}

Usage Usage::from_json(const json::JsonValue& usage_json,
                       bool cache_counted_in_prompt) {
    if (!usage_json.is_object()) return Usage();

    /* Первое найденное имя побеждает: у провайдера обе раскладки сразу
     * не бывает, а «какая-то из двух» лучше, чем ноль. */
    const auto pick = [](const json::JsonValue& v, const char* a,
                         const char* b) -> long long {
        if (v.has(a)) return v.get_int(a, 0);
        if (v.has(b)) return v.get_int(b, 0);
        return 0;
    };

    const long long input_tokens = pick(usage_json, "prompt_tokens", "input_tokens");
    const long long output_tokens =
        pick(usage_json, "completion_tokens", "output_tokens");

    long long cache_read = 0;
    if (usage_json.has("prompt_tokens_details")) {
        cache_read = usage_json.get_int("prompt_tokens_details.cached_tokens", 0);
    }
    if (cache_read == 0) {
        cache_read = pick(usage_json, "cache_read_input_tokens",
                          "cache_read_tokens");
    }
    const long long cache_write =
        pick(usage_json, "cache_creation_input_tokens", "cache_write_tokens");

    Usage u = Usage::from_provider(input_tokens, output_tokens,
                                   cache_read, cache_write,
                                   cache_counted_in_prompt);

    long long reasoning = 0;
    if (usage_json.has("completion_tokens_details")) {
        reasoning = usage_json.get_int("completion_tokens_details.reasoning_tokens", 0);
    }
    if (reasoning == 0) {
        if (usage_json.has("reasoning_tokens")) {
            reasoning = usage_json.get_int("reasoning_tokens", 0);
        }
    }
    u.set_reasoning(reasoning);
    return u;
}

void Usage::set_reasoning(long long tokens) {
    const long long v = non_negative(tokens);
    reasoning = std::min(v, output);
}

const char* llm_event_kind_name(LlmEventKind kind) {
    switch (kind) {
        case LlmEventKind::StepStart:        return "step_start";
        case LlmEventKind::TextStart:        return "text_start";
        case LlmEventKind::TextDelta:        return "text_delta";
        case LlmEventKind::TextEnd:          return "text_end";
        case LlmEventKind::ReasoningStart:   return "reasoning_start";
        case LlmEventKind::ReasoningDelta:   return "reasoning_delta";
        case LlmEventKind::ReasoningEnd:     return "reasoning_end";
        case LlmEventKind::ToolInputStart:   return "tool_input_start";
        case LlmEventKind::ToolInputDelta:   return "tool_input_delta";
        case LlmEventKind::ToolInputEnd:     return "tool_input_end";
        case LlmEventKind::ToolCall:         return "tool_call";
        case LlmEventKind::ToolResult:       return "tool_result";
        case LlmEventKind::ToolError:        return "tool_error";
        case LlmEventKind::StepFinish:       return "step_finish";
        case LlmEventKind::Finish:           return "finish";
        case LlmEventKind::ProviderError:    return "provider_error";
        case LlmEventKind::kCount:           break;
    }
    return "unknown";
}

const char* failure_kind_name(FailureKind kind) {
    switch (kind) {
        case FailureKind::None:     return "нет";
        case FailureKind::Provider: return "провайдер";
        case FailureKind::Tool:     return "инструмент";
        case FailureKind::Aborted:  return "прервано";
    }
    return "неизвестно";
}

/* --- Фабрики --- */

LlmEvent LlmEvent::step_start() {
    return LlmEvent(LlmEventKind::StepStart);
}

LlmEvent LlmEvent::text_start() {
    return LlmEvent(LlmEventKind::TextStart);
}

LlmEvent LlmEvent::text_delta(std::string delta) {
    LlmEvent e(LlmEventKind::TextDelta);
    e.text_ = std::move(delta);
    return e;
}

LlmEvent LlmEvent::text_end() {
    return LlmEvent(LlmEventKind::TextEnd);
}

LlmEvent LlmEvent::reasoning_start() {
    return LlmEvent(LlmEventKind::ReasoningStart);
}

LlmEvent LlmEvent::reasoning_delta(std::string delta) {
    LlmEvent e(LlmEventKind::ReasoningDelta);
    e.text_ = std::move(delta);
    return e;
}

LlmEvent LlmEvent::reasoning_end() {
    return LlmEvent(LlmEventKind::ReasoningEnd);
}

LlmEvent LlmEvent::tool_input_start(std::string call_id,
                                    std::string tool_name) {
    LlmEvent e(LlmEventKind::ToolInputStart);
    e.call_id_ = std::move(call_id);
    e.tool_name_ = std::move(tool_name);
    return e;
}

LlmEvent LlmEvent::tool_input_delta(std::string call_id, std::string delta) {
    LlmEvent e(LlmEventKind::ToolInputDelta);
    e.call_id_ = std::move(call_id);
    e.text_ = std::move(delta);
    return e;
}

LlmEvent LlmEvent::tool_input_end(std::string call_id) {
    LlmEvent e(LlmEventKind::ToolInputEnd);
    e.call_id_ = std::move(call_id);
    return e;
}

LlmEvent LlmEvent::tool_call(std::string call_id, std::string tool,
                             json::JsonValue args) {
    LlmEvent e(LlmEventKind::ToolCall);
    e.call_id_ = std::move(call_id);
    e.tool_name_ = std::move(tool);
    e.arguments_ = std::move(args);
    return e;
}

LlmEvent LlmEvent::tool_result(std::string call_id, ToolOutput output) {
    LlmEvent e(LlmEventKind::ToolResult);
    e.call_id_ = std::move(call_id);
    e.tool_output_ = std::move(output);
    return e;
}

LlmEvent LlmEvent::tool_error(std::string call_id, std::string error) {
    LlmEvent e(LlmEventKind::ToolError);
    e.call_id_ = std::move(call_id);
    e.text_ = std::move(error);
    return e;
}

LlmEvent LlmEvent::step_finish() {
    return LlmEvent(LlmEventKind::StepFinish);
}

LlmEvent LlmEvent::finish(std::string finish_reason) {
    LlmEvent e(LlmEventKind::Finish);
    e.finish_reason_ = std::move(finish_reason);
    return e;
}

LlmEvent LlmEvent::finish(std::string finish_reason, Usage usage) {
    LlmEvent e(LlmEventKind::Finish);
    e.finish_reason_ = std::move(finish_reason);
    e.usage_ = std::move(usage);
    return e;
}

LlmEvent LlmEvent::provider_error(std::string error) {
    LlmEvent e(LlmEventKind::ProviderError);
    e.text_ = std::move(error);
    return e;
}

/* --- Аксессоры --- */

const std::string& LlmEvent::delta() const {
    if (kind_ != LlmEventKind::TextDelta &&
        kind_ != LlmEventKind::ReasoningDelta &&
        kind_ != LlmEventKind::ToolInputDelta) {
        return empty_string();
    }
    return text_;
}

const std::string& LlmEvent::call_id() const {
    switch (kind_) {
        case LlmEventKind::ToolInputStart:
        case LlmEventKind::ToolInputDelta:
        case LlmEventKind::ToolInputEnd:
        case LlmEventKind::ToolCall:
        case LlmEventKind::ToolResult:
        case LlmEventKind::ToolError:
            return call_id_;
        default:
            return empty_string();
    }
}

const std::string& LlmEvent::tool_name() const {
    if (kind_ != LlmEventKind::ToolCall &&
        kind_ != LlmEventKind::ToolInputStart) {
        return empty_string();
    }
    return tool_name_;
}

const json::JsonValue& LlmEvent::arguments() const {
    if (kind_ != LlmEventKind::ToolCall) return empty_json();
    return arguments_;
}

const ToolOutput& LlmEvent::tool_output() const {
    static const ToolOutput kEmpty;
    if (kind_ != LlmEventKind::ToolResult) return kEmpty;
    return tool_output_;
}

const std::string& LlmEvent::finish_reason() const {
    if (kind_ != LlmEventKind::Finish) return empty_string();
    return finish_reason_;
}

const Usage& LlmEvent::usage() const {
    if (kind_ != LlmEventKind::Finish) return empty_usage();
    return usage_;
}

const std::string& LlmEvent::error() const {
    if (kind_ != LlmEventKind::ToolError &&
        kind_ != LlmEventKind::ProviderError) {
        return empty_string();
    }
    return text_;
}

/* ======================================================================
 * LlmResponse::reduce (И5.3)
 * ====================================================================== */

void LlmResponse::clear() {
    *this = LlmResponse();
}

void LlmResponse::drain_utf8_tail(std::string& out, std::string& tail) {
    /* Дельты приходят БАЙТАМИ, и один символ может прийти двумя
     * дельтами: «С» + «лучай» — это два события с одним символом
     * посередине. Если склеивать байты наивно, в тексте появляется
     * обрыв, а в истории — битый UTF-8, который молча ломает и разбор,
     * и вывод в UI.
     *
     * Поэтому из буфера выносится только то, что уже не может оказаться
     * началом символа; недобранный хвост ждёт следующую дельту. */
    size_t pos = 0;
    while (pos < tail.size()) {
        size_t length = 0;
        if (text::utf8_sequence_at(tail, pos, length)) {
            pos += length;
            continue;
        }
        const size_t need = utf8_expected_len(
            static_cast<unsigned char>(tail[pos]));
        if (need > tail.size() - pos) {
            /* Байтов меньше, чем обещает первый: это НАЧАЛО символа,
             * разрезанное дельтой. Ждём продолжения — иначе в тексте
             * появился бы обрыв, а в истории битый UTF-8, который тихо
             * ломает разбор и вывод в UI. */
            break;
        }
        /* Символ неполучен либо бит (0xFF, «0xE0 0x5A …»): дальше он
         * валидным не станет. Идём дальше РАЗБОРЕМ, иначе этот байт
         * держал бы весь остаток ответа в хвосте до конца шага, и агент
         * выглядел бы зависшим без единой ошибки. Сам байт при этом
         * попадает в текст как есть — потеря видна, и её снимет
         * sanitize_utf8 при записи в историю. */
        ++pos;
    }
    if (pos == 0) return;
    out.append(tail, 0, pos);
    tail.erase(0, pos);
}

LlmToolCall& LlmResponse::call_for(const std::string& call_id) {
    for (LlmToolCall& c : calls_) {
        if (c.call_id == call_id) return c;
    }
    LlmToolCall c;
    c.call_id = call_id;
    calls_.push_back(std::move(c));
    return calls_.back();
}

LlmToolCall* LlmResponse::find_call(const std::string& call_id) {
    for (LlmToolCall& c : calls_) {
        if (c.call_id == call_id) return &c;
    }
    return nullptr;
}

void LlmResponse::commit_pending_calls() {
    /* Разбор аргументов делается ОДИН раз — на закрытии входа. Разбор
     * каждой дельты потребовал бы частичного JSON-парсера, а провайдер
     * всё равно может прислать аргументы в любом порядке ключей. */
    for (LlmToolCall& c : calls_) {
        if (c.input_closed || c.from_native_call) continue;
        c.input_closed = true;
        if (c.name.empty()) {
            /* Провайдер прислал аргументы, но не имя инструмента. Имя
             * остаётся пустым, и вызов окажется неисполнимым: честнее
             * закрыть его ошибкой, чем отдать циклу вызов без имени. */
            c.failed = true;
            c.finished = true;
            c.error =
                "провайдер прислал аргументы вызова без имени инструмента";
            continue;
        }
        if (c.raw_input.empty()) {
            c.failed = true;
            c.finished = true;
            c.error = "вызов инструмента без аргументов";
            continue;
        }
        std::string parse_error;
        json::JsonValue args;
        if (!json::JsonValue::parse(c.raw_input, args, &parse_error)) {
            /* Мусор в аргументах не должен молча исчезнуть: вызов
             * закрывается ошибкой с сохранением сырья, и цикл отдаст
             * модели настоящий результат вызова вместо протокольной
             * ошибки (смысл — как у repairToolCall в И12.3). */
            c.failed = true;
            c.finished = true;
            c.error = "не удалось разобрать аргументы (" + parse_error +
                      "): " + text::utf8_prefix(c.raw_input, 200);
            continue;
        }
        c.arguments = std::move(args);
    }
}

void LlmResponse::reduce(LlmResponse& state, const LlmEvent& event) {
    switch (event.kind()) {
        case LlmEventKind::StepStart:
            break;

        case LlmEventKind::TextStart:
            break;

        case LlmEventKind::TextDelta:
            state.text_tail_ += event.delta();
            state.drain_utf8_tail(state.text_, state.text_tail_);
            ++state.text_deltas_;
            break;

        case LlmEventKind::TextEnd:
            /* Закрытие блока — единственное место, где можно отдать
             * недобранный хвост: дальше его уже не примкнут. Провайдер,
             * оборвавший ответ посреди символа, даст битый байт в конце,
             * и его снимет sanitize_utf8 при записи в историю. Тихой
             * потери символа здесь нет, потеря видна. */
            if (!state.text_tail_.empty()) {
                state.text_ += state.text_tail_;
                state.text_tail_.clear();
            }
            break;

        case LlmEventKind::ReasoningStart:
            break;

        case LlmEventKind::ReasoningDelta:
            state.reasoning_tail_ += event.delta();
            state.drain_utf8_tail(state.reasoning_, state.reasoning_tail_);
            break;

        case LlmEventKind::ReasoningEnd:
            if (!state.reasoning_tail_.empty()) {
                state.reasoning_ += state.reasoning_tail_;
                state.reasoning_tail_.clear();
            }
            break;

        case LlmEventKind::ToolInputStart:
            /* Имя инструмента приходит здесь: аргументы пойдут
             * дельтами, отдельного ToolCall может не быть. */
            if (!event.tool_name().empty()) {
                state.call_for(event.call_id()).name = event.tool_name();
            }
            break;

        case LlmEventKind::ToolInputDelta:
            /* Сырые байты склеиваются как есть: разбор будет один, на
             * ToolInputEnd, поэтому символ, разрезанный дельтой, здесь
             * не мешает. */
            state.call_for(event.call_id()).raw_input += event.delta();
            break;

        case LlmEventKind::ToolInputEnd:
            /* Здесь только коммит: сам признак «ввод закрыт» ставит
             * commit_pending_calls, иначе вызов, чей ввод закрылся
             * прямо сейчас, был бы пропущен как уже обработанный — и
             * остался бы вечно без разобранных аргументов. */
            state.commit_pending_calls();
            break;

        case LlmEventKind::ToolCall: {
            LlmToolCall& c = state.call_for(event.call_id());
            c.name = event.tool_name();
            c.arguments = event.arguments();
            c.input_closed = true;
            c.from_native_call = true;
            break;
        }

        case LlmEventKind::ToolResult: {
            /* Результат вызова, которого не было, — расхождение
             * протокола (или разные идентификаторы у адаптера и у
             * провайдера). Молча привязать его к чему попало значило бы
             * потерять результат; вместо этого он остаётся видимым
             * ошибкой. */
            LlmToolCall* c = state.find_call(event.call_id());
            if (!c) {
                LlmToolCall& fresh = state.call_for(event.call_id());
                fresh.finished = true;
                fresh.failed = true;
                fresh.error = "результат вызова, которого не было: " +
                              event.call_id();
                break;
            }
            c->output = event.tool_output();
            c->finished = true;
            c->failed = false;
            break;
        }

        case LlmEventKind::ToolError: {
            LlmToolCall* c = state.find_call(event.call_id());
            if (!c) {
                LlmToolCall& fresh = state.call_for(event.call_id());
                fresh.finished = true;
                fresh.failed = true;
                fresh.error = "ошибка вызова, которого не было: " +
                              event.call_id() + ": " + event.error();
                break;
            }
            c->error = event.error();
            c->finished = true;
            c->failed = true;
            /* И6.8: упавший инструмент — это тоже сбой хода, но не
             * провайдера. Разница видна пользователю: отказ провайдера
             * повторяют, упавший инструмент — смотрят, что он писал в
             * ответе. */
            if (state.failure_ == FailureKind::None) {
                state.failure_ = FailureKind::Tool;
            }
            break;
        }

        case LlmEventKind::StepFinish:
            /* Провайдер, присылающий аргументы дельтами, может не
             * прислать ToolInputEnd — тогда вызов остался бы навсегда
             * «незакрытым вводом» и потерялся бы целиком. Шаг закрыт —
             * значит, аргументов больше не будет. */
            state.commit_pending_calls();
            break;

        case LlmEventKind::Finish:
            state.commit_pending_calls();
            if (!state.text_tail_.empty()) {
                state.text_ += state.text_tail_;
                state.text_tail_.clear();
            }
            if (!state.reasoning_tail_.empty()) {
                state.reasoning_ += state.reasoning_tail_;
                state.reasoning_tail_.clear();
            }
            state.finish_reason_ = event.finish_reason();
            state.usage_ = event.usage();
            break;

        case LlmEventKind::ProviderError:
            state.error_ = event.error();
            /* И6.8: вид сбоя ставится ЗДЕСЬ, тем же событием, что и текст.
             * Классифицировать текст ошибки в другом месте означало бы
             * второй разбор того же самого — и рано или поздно он разошёлся
             * бы с событием (например, «429» в сообщении пользователя). */
            state.failure_ = FailureKind::Provider;
            break;

        case LlmEventKind::kCount:
            break;
    }
}

bool LlmResponse::has_open_tool_call() const {
    for (const LlmToolCall& c : calls_) {
        if (!c.finished) return true;
    }
    return false;
}

bool LlmResponse::empty() const {
    return text_.empty() && reasoning_.empty() && calls_.empty() &&
           error_.empty();
}

} // namespace coder
