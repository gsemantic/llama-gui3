// llm_source.cpp — источник событий (И5.7).

#include "llm_source.h"
#include "json_utils.h"
#include "tool_protocol.h"

namespace coder {
namespace llm_source {
namespace {

/* Идентификатор вызова, который модель написала в тексте.
 *
 * У текстового протокола он синтетический: провайдер номера вызовов не
 * присылает, потому что вызовов в его потоке нет — есть блок в строке.
 * Синтезировать его всё равно обязательно: без call_id результат
 * инструмента не к чему привязать, а состояние вызова в части сообщения
 * (И5.4) живёт именно по нему. */
const char* kTextCallId = "call_0";

/* Последняя реплика пользователя — единственное, чем одногилый хост
 * (без llm_chat) заменяет историю. Берётся последняя, а не первая:
 * в разбиении хода (core/message.h) результат инструмента — тоже
 * реплика пользователя, и именно её модель должна продолжать. */
std::string last_user_message(const std::vector<ModelMessage>& history) {
    std::string last;
    for (const ModelMessage& m : history) {
        if (m.role == kRoleUser) last = m.content;
    }
    return last;
}

/* Модель пишет пустую строку перед блоком вызова, поэтому «текст без
 * блока» заканчивается переводом строки. В транскрипте части склеиваются
 * через «\n\n» сами (core/message.h), и хвостовая пустая строка была бы
 * вторым переводом строки в каждом запросе — и видна в UI как пустой
 * абзац. Обрезаем только хвост: ведущие пробелы могут быть отступом в
 * примере кода, и их потеря изменила бы смысл текста. */
std::string without_trailing_blanks(std::string s) {
    size_t end = s.size();
    while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t' ||
                       s[end - 1] == '\r' || s[end - 1] == '\n')) {
        --end;
    }
    s.resize(end);
    return s;
}

} // namespace

void reply_to_events(const LlmReply& reply, std::vector<LlmEvent>& events) {
    events.push_back(LlmEvent::step_start());

    /* Блок вызова вырезается ДО текста: иначе протокол уехал бы в
     * транскрипт дважды — и как текст, и как вызов. */
    std::string rest;
    const std::string block = extract_action(reply.content, rest);
    const std::string text = without_trailing_blanks(rest);

    if (!text.empty()) {
        events.push_back(LlmEvent::text_start());
        events.push_back(LlmEvent::text_delta(text));
        events.push_back(LlmEvent::text_end());
    }

    if (!block.empty()) {
        json::JsonValue args;
        const bool parsed = parse_action(block, args);
        const std::string name = args.get_string("tool");
        const std::string call_id(kTextCallId);

        /* Сырой блок едет событием ДЕЛЬТЫ, а не только полем вызова:
         * модель на следующем шаге должна увидеть тот же текст, который
         * написала сама (см. MessagePart::raw_call). Разбор при этом
         * делает адаптер, а не свёртка: свёртка разбирает JSON, а
         * протокол допускает ещё и legacy-форму «TOOL: …\nPATH: …».
         * Порядок поэтому такой: дельта, потом ToolCall (он помечает
         * вызов нативным, и следующая за ним коммитация не пытается
         * разобрать блок как JSON), потом закрытие ввода. */
        events.push_back(LlmEvent::tool_input_start(call_id, name));
        events.push_back(LlmEvent::tool_input_delta(call_id, block));
        if (parsed) {
            events.push_back(LlmEvent::tool_call(call_id, name, args));
            events.push_back(LlmEvent::tool_input_end(call_id));
        } else {
            events.push_back(LlmEvent::tool_input_end(call_id));
            /* Неразобранный блок — исход вызова, а не молчание. Молчание
             * было бы хуже всего: модель увидела бы в истории пустой
             * вызов и на следующем шаге повторила бы тот же блок. */
            events.push_back(LlmEvent::tool_error(
                call_id, "не удалось разобрать блок вызова: " +
                             text::utf8_prefix(block, 200)));
        }
    }

    events.push_back(LlmEvent::step_finish());
    /* finish_reason приводится к тому, чем ход ЯВЛЯЕТСЯ, а не тем, что
     * прислал хост. Хост про блок вызова не знает (он не разбирает
     * протокол) и честно пишет «stop», а условие завершения (И5.8) смотрит
     * на finish: без этой правки ход с вызовом выглядел бы финальным
     * ответом, и задача закрылась бы с невыполненным инструментом.
     *
     * Обход по «stop»: если провайдер оборвал ответ по длине (length), то
     * finish=length честнее, чем «tool-calls» — блок мог быть не дописан. */
    std::string finish = reply.finish_reason;
    if (!block.empty() && (finish.empty() || finish == "stop")) {
        finish = "tool-calls";
    }
    /* usage едет вместе с закрытием ответа (И5.2), а не с каждой
     * дельтой: провайдер, присылающий метрики в последнем чанке, иначе
     * дал бы сто накоплений. Числа провайдера переводятся один раз
     * здесь — потребителям достаётся готовый Usage с неперекрывающимися
     * счётчиками, и никто не складывает prompt с кэшем сам. */
    events.push_back(LlmEvent::finish(
        finish, Usage::from_provider(reply.prompt_tokens,
                                     reply.completion_tokens)));
}

bool fetch(const HostCallbacks& cb, const std::string& sys_prompt,
           const std::vector<ModelMessage>& history,
           std::vector<LlmEvent>& events) {
    LlmReply reply;
    bool ok = false;
    if (cb.llm_chat) {
        ok = cb.llm_chat(sys_prompt, history, reply);
    } else if (cb.llm_complete) {
        std::string resp;
        ok = cb.llm_complete(sys_prompt, last_user_message(history), resp);
        if (ok) {
            reply.content = resp;
            /* Метрики одногилого хоста неизвестны, а выдумывать их
             * нельзя: панель токенов (И11.9) и оценка контекста (И7)
             * считали бы выдуманное как настоящее. */
            reply.finish_reason = "stop";
        }
    }
    if (!ok) {
        events.push_back(LlmEvent::provider_error(
            reply.error.empty() ? "не ответил" : reply.error));
        return false;
    }
    reply_to_events(reply, events);
    return true;
}

LlmResponse fold(const std::vector<LlmEvent>& events) {
    LlmResponse response;
    for (const LlmEvent& e : events) {
        LlmResponse::reduce(response, e);
    }
    return response;
}

} // namespace llm_source
} // namespace coder
