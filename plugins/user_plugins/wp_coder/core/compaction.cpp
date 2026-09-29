/*
 * compaction.cpp — математика переполнения контекста (И7.1).
 *
 * Порядок проверок в usable() — часть правила, а не оформление:
 * сначала «известно ли окно», потом «какой лимит берём», и только потом
 * вычитание. Обратный порядок дал бы при context == 0 (провайдер не
 * объявил) результат max(0, 0 - max_output) = 0 — то есть «мест нет
 * совсем», и is_overflow() объявил бы переполнение на пустой истории.
 * Ноль здесь значит «считать нечем», и это разные вещи, поэтому
 * разделены (см. шапку compaction.h).
 */

#include "core/compaction.h"

/* text::utf8_prefix — резать по границе символа, а не по границе байта. */
#include "core/json_utils.h"
#include "core/prompts.h"
#include "core/tool_protocol.h"   /* extract_action/parse_action: чужой вызов */

#include <cstddef>

namespace coder {
namespace compaction {

long long reserve_for(const ModelLimits& limits, const CompactionConfig& cfg) {
    /* Резерв из настроек важнее умолчания: это и есть смысл настройки. */
    if (cfg.reserved > 0) return cfg.reserved;
    /* Умолчание — буфер, ограниченный лимитом ответа: держать в резерве
     * больше, чем весь ответ, бессмысленно — места под него всё равно не
     * появится. */
    if (limits.max_output <= 0) return 0;
    const long long buffer = limits::kCompactionBuffer;
    return limits.max_output < buffer ? limits.max_output : buffer;
}

long long usable(const ModelLimits& limits, const CompactionConfig& cfg) {
    /* Окно неизвестно — порога не существует. */
    if (limits.context <= 0) return 0;

    long long left;
    if (limits.input > 0) {
        /* Вход ограничен провайдером отдельно: вычитаем резерв под
         * сводку, а не лимит ответа (см. две ветки в шапке). */
        left = limits.input - reserve_for(limits, cfg);
    } else {
        /* Известно только окно: вычитаем ПОЛНЫЙ лимит ответа, иначе
         * история доросла бы до context - буфера, а запрос с длинным
         * ответом вышел бы за окно. */
        left = limits.context - limits.max_output;
    }
    return left > 0 ? left : 0;
}

bool is_overflow(const ModelLimits& limits, const CompactionConfig& cfg,
                 long long used_tokens) {
    /* Два выхода до вычисления порога, и оба — про «не объявлять», то
     * есть про честность: выключено пользователем; либо лимиты неизвестны
     * (usable() == 0), либо места не осталось вовсе — и тогда сжимать
     * нечем, а повторное сжатие ничего не освободит, то есть вызывающий
     * зациклился бы. Различие причин названо в комментарии намеренно:
     * строки ради него второй раз НЕТ — снаружи обе выглядят одинаково
     * (проверка одна), и добавлять «для ясности» вторую значило бы
     * завести код, который ничего не меняет. */
    if (!cfg.auto_compact) return false;
    const long long room = usable(limits, cfg);
    if (room <= 0) return false;
    return used_tokens >= room;
}

/* --- И7.2: откуда берётся число --- */

const char* token_source_name(TokenSource s) {
    switch (s) {
        case TokenSource::Measured:  return "измерено";
        case TokenSource::Estimated: return "оценка";
    }
    return "неизвестно";
}

long long tokens_from_chars(long long chars) {
    if (chars <= 0) return 0;
    /* Math.round(n / 4) = (n + 2) / 4 для неотрицательных n. */
    return (chars + limits::kCharsPerToken / 2) / limits::kCharsPerToken;
}

long long estimate_tokens(const std::string& text) {
    return tokens_from_chars(static_cast<long long>(text.size()));
}

long long estimate_history_tokens(const std::vector<Message>& history) {
    return estimate_history_tokens(history, 0, history.size());
}

ContextUsage context_usage(long long measured_input,
                           const std::vector<Message>& history) {
    if (measured_input > 0) return {measured_input, TokenSource::Measured};
    return {estimate_history_tokens(history), TokenSource::Estimated};
}

/* --- И7.3: выбор хвоста --- */

long long estimate_history_tokens(const std::vector<Message>& history,
                                  size_t from, size_t to) {
    if (from >= to) return 0;
    /* Срез копируется: to_model_messages принимает историю целиком, а
     * второй такой же сборщик транскрипта означал бы второй протокол
     * (D2 в новом месте). Копия не бесплатна, но срез — это один-два
     * хода, и вызывающих на одну выборку порядка десятка. */
    std::vector<Message> slice(history.begin() + static_cast<std::ptrdiff_t>(from),
                               history.begin() + static_cast<std::ptrdiff_t>(to));
    long long chars = 0;
    for (const ModelMessage& m : to_model_messages(slice)) {
        chars += static_cast<long long>(m.content.size());
    }
    return tokens_from_chars(chars);
}

namespace {

/* Ход: сообщения [start, end). */
struct TurnRange {
    size_t start = 0;
    size_t end = 0;
};

/* Реплика пользователя, породившая ход. Обёртка сводки (часть
 * compaction) ходом НЕ считается — см. решение 1 в compaction.h.
 *
 * Реплика автопродолжения (И7.7) ходом считается, и это ОБЩЕЕ правило
 * (пользовательская реплика без compaction-части), а не частный случай:
 * так же и в порте.
 *
 * Отдельно записано то, что стоило проверки мутацией: это различение
 * НЕ влияет на выбор хвоста. Мутация «реплика продолжения ходом не
 * считается» не ломает ни одну проверку, потому что разрез хода
 * (решение 3) приходит к той же границе: ход, не влезающий в порог,
 * разрезается начиная со второго сообщения, и реплика продолжения
 * оказывается началом уцелевшего куска в обоих случаях. Значит,
 * свойство, которое реально держится, — не «считается ходом», а
 * «хвост начинается с реплики продолжения»; оно и проверяется. */
bool starts_a_turn(const Message& m) {
    if (!m.is_user()) return false;
    for (const MessagePart& p : m.parts) {
        if (p.is(PartKind::Compaction)) return false;
    }
    return true;
}

std::vector<TurnRange> turns_of(const std::vector<Message>& history) {
    std::vector<TurnRange> out;
    for (size_t i = 0; i < history.size(); ++i) {
        if (!starts_a_turn(history[i])) continue;
        out.push_back({i, history.size()});
    }
    /* Конец хода — начало следующего; последний ход тянется до конца
     * истории. Обрывать его на «последнем сообщении пользователя» нельзя:
     * там может быть хвостовой ответ агента (И5.9), и он принадлежит
     * тому же ходу. */
    for (size_t i = 0; i + 1 < out.size(); ++i) out[i].end = out[i + 1].start;
    return out;
}

/* Первое начало среза, хвост которого влезает в budget. Возвращает
 * history.size(), если не влезает ни один.
 *
 * Перебор идёт от начала хода к его концу, и возвращается ПЕРВый
 * подошедший: значит уцелевает самый длинный возможный кусок хода, а не
 * самый короткий. */
size_t split_within_turn(const std::vector<Message>& history,
                         const TurnRange& turn, long long budget) {
    if (budget <= 0) return history.size();
    for (size_t start = turn.start + 1; start < turn.end; ++start) {
        if (estimate_history_tokens(history, start, turn.end) <= budget) {
            return start;
        }
    }
    return history.size();
}

} // namespace

long long preserve_recent_budget(const ModelLimits& limits,
                                 const CompactionConfig& cfg) {
    /* Настройка важнее умолчания: иначе её нельзя было бы отличить от
     * отсутствия. */
    if (cfg.preserve_recent_tokens > 0) return cfg.preserve_recent_tokens;
    const long long quarter = usable(limits, cfg) / 4;
    if (quarter < limits::kMinPreserveRecentTokens) {
        return limits::kMinPreserveRecentTokens;
    }
    if (quarter > limits::kMaxPreserveRecentTokens) {
        return limits::kMaxPreserveRecentTokens;
    }
    return quarter;
}

Selection select_to_compact(const std::vector<Message>& history,
                            const ModelLimits& limits,
                            const CompactionConfig& cfg) {
    Selection sel;
    sel.budget = preserve_recent_budget(limits, cfg);
    /* Голова по умолчанию — ВСЯ история, а не пустота. Ответ «сжимаем
     * всё» и ответ «сжимать нечего» совпадают именно в этом: в порте
     * оба возвращают { head: messages, tail: undefined }. Первая версия
     * оставляла head пустым на ранних выходах, то есть вызывающий
     * суммировал бы пустоту и получал вывод «не о чем рассказывать» —
     * молчаливая потеря всей истории. */
    sel.head = history;
    /* Порог считается ДО ранних выходов намеренно: вызывающий показывает
     * его в панели, и «хвост не оставляем» не должно выглядеть как
     * «порога нет». */
    if (cfg.tail_turns == 0) return sel;          /* хвоста нет вовсе */
    if (history.empty()) return sel;

    std::vector<TurnRange> recent = turns_of(history);
    if (recent.empty()) return sel;               /* ходов нет — жать нечего */
    if (cfg.tail_turns > 0 && (size_t)cfg.tail_turns < recent.size()) {
        recent.erase(recent.begin(),
                     recent.end() - static_cast<size_t>(cfg.tail_turns));
    }

    /* Обход от новых к старым. size_t, а не long: «хвоста пока нет» — это
     * конец истории, и он же совпадает с началом всего хвоста, когда
     * уцелела вся история (см. проверку после цикла). */
    size_t keep_from = history.size();
    long long total = 0;
    for (size_t i = recent.size(); i-- > 0;) {
        const TurnRange& t = recent[i];
        const long long size =
            estimate_history_tokens(history, t.start, t.end);
        if (total + size <= sel.budget) {
            total += size;
            keep_from = t.start;
            continue;
        }
        /* Ход не влез: уцелеет его собственный хвост (решение 3).
         * Если и он не влезает — уцелеет то, что уже накоплено, а
         * перебор обрывается: идти к более старым ходам после
         * неудачи бессмысленно, иначе хвост перестал бы быть
         * НЕПРЕРЫВНЫМ (старый ход влез бы, а свежий между ним и
         * началом — нет, и пропуск в хвосте разорвал бы смысл
         * «последние ходы»). */
        const size_t split = split_within_turn(history, t, sel.budget - total);
        if (split != history.size()) keep_from = split;
        break;
    }

    /* Решение 4: если уцелела вся история — сжимать нечего, и ответ
     * «сжать всё» был бы выдумкой (сводка из того, что и так умещается).
     * Тот же пустой хвост означает уже другое — «не влезает даже свежий
     * ход», и тогда сжатие обязано быть полным. Различие — в том, звали
     * ли мы select вообще: вызывающий приходит сюда по is_overflow. */
    if (keep_from == history.size() || keep_from == 0) return sel;

    const auto at = static_cast<std::ptrdiff_t>(keep_from);
    sel.head.assign(history.begin(), history.begin() + at);
    sel.tail.assign(history.begin() + at, history.end());
    sel.tail_tokens = estimate_history_tokens(history, keep_from, history.size());
    return sel;
}

/* --- И7.4: плоский транскрипт для сводщика --- */

namespace {

/* Результат инструмента усекается: сводка не должна раздуваться тем же
 * выводом, ради устранения которого её и затеяли. Режется по границе
 * UTF-8 — битый байт в промпте портит текст тихо и без сообщения. */
std::string capped(const std::string& s) {
    if (s.size() <= static_cast<size_t>(limits::kSummaryToolOutputChars)) {
        return s;
    }
    std::string head = text::utf8_prefix(
        s, static_cast<size_t>(limits::kSummaryToolOutputChars));
    head += "\n[обрезано, всего " + std::to_string(s.size()) + " символов]";
    return head;
}

/* Ловушка, на которую уже наступали: has_result() означает «исход есть»
 * (результат ИЛИ отказ), а не «результат есть». Проверка по нему отправила
 * бы отказ в строку результата, и сводка получила бы "[Tool result]: " без
 * текста вместо "[Tool error]: отказано" — то есть потеряла бы ровно то,
 * из-за чего повторно ходить по инструментам не нужно. Поэтому вид исхода
 * проверяется по state(), как это уже сделано в tool_result_text
 * (core/message.cpp). */
/* Строки одного сообщения; пустое сообщение не даёт ни строки, ни
 * пустого разделителя — иначе в промпте сводщика появляются дыры,
 * выглядящие как «здесь был ход, о котором забыли». */
std::vector<std::string> lines_of(const Message& m) {
    std::vector<std::string> lines;
    const bool as_user = m.is_user();
    for (const MessagePart& p : m.parts) {
        if (p.is(PartKind::Tool)) {
            /* Вызов — реплика ассистента даже внутри сообщения
             * пользователя (часть хранит и вызов, и исход, см.
             * to_model_messages). */
            const std::string call = tool_call_text(p);
            if (!call.empty()) {
                lines.push_back("[Assistant tool call]: " + call);
            }
            if (p.state() == ToolState::Completed) {
                lines.push_back("[Tool result]: " + capped(p.output().output));
            } else if (p.state() == ToolState::Error) {
                /* Ошибку НЕ усекаем: это отказ режима или разрешения,
                 * а не вывод команды (вывод идёт в результат, где он уже
                 * усечён). Так же и в порту. */
                lines.push_back("[Tool error]: " + p.error());
            }
            /* Pending и Running не дают строки исхода: вызов есть, а
             * ответа ещё нет, и дописывать нечего. */
            continue;
        }
        if (p.is(PartKind::Reasoning)) {
            if (!p.text().empty()) {
                lines.push_back("[Assistant reasoning]: " + p.text());
            }
            continue;
        }
        if (p.is(PartKind::Text)) {
            if (!p.text().empty()) {
                lines.push_back(std::string(as_user ? "[User]: " : "[Assistant]: ") +
                                p.text());
            }
            continue;
        }
        /* Часть compaction сюда НЕ попадает намеренно (И7.6): прошлая
         * сводка идёт в сводщику отдельной полосой <prior-summary>, а как
         * строка `[User]:` она читалась бы словами человека — при том,
         * что промпт просит считать разговор новее сводки.
         *
         * Реплика автопродолжения (И7.7) не попадает по той же причине,
         * что и она: `[User]: Continue if you have next steps…` — это
         * слова плагина, поданные как слова человека, а промпт сводщика
         * прямо запрещает приписывать человеку то, чего он не говорил.
         * В порте она в сводку попадает, и это не ошибка там, где сводку
         * видит только модель; у нас сводка — единственное, что увидит
         * и человек в UI (И11), поэтому лишнее «человек сказал
         * „продолжай“» в ней обманывало бы и его.
         *
         * Служебные виды (StepStart, StepFinish, Retry, Patch, Subtask)
         * молчат по третьей причине: они не несут содержания, а формат
         * для них не описан ни здесь, ни в промпте сводщика. */
    }
    return lines;
}

} // namespace

std::string serialize_for_summary(const std::vector<Message>& history) {
    std::string out;
    for (const Message& m : history) {
        const std::vector<std::string> lines = lines_of(m);
        if (lines.empty()) continue;
        if (!out.empty()) out += "\n\n";
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i) out += "\n";
            out += lines[i];
        }
    }
    return out;
}

/* --- И7.6: прошлая сводка --- */

std::string previous_summary(const std::vector<Message>& history) {
    std::string out;
    /* Обход до конца и присваивание, а не выход на первой найденной:
     * берётся ПОСЛЕДНЯЯ непустая сводка, и она содержит все предыдущие
     * (каждая следующая составлена с предыдущей в <prior-summary>, см.
     * шапку compaction.h). Обратный порядок взял бы самую старую и
     * выбросил бы всё, сделанное между сжатиями, — молча, потому что
     * промпт сводщика отработал бы и выдал правдоподобную сводку. */
    for (const Message& m : history) {
        for (const MessagePart& p : m.parts) {
            if (!p.is(PartKind::Compaction)) continue;
            if (p.text().empty()) continue;
            out = p.text();
        }
    }
    return out;
}

/* --- И7.5: агент-сводщик --- */

const char* compaction_outcome_name(CompactionOutcome o) {
    switch (o) {
        case CompactionOutcome::Continue: return "продолжаем";
        case CompactionOutcome::Stop:     return "стоп";
        case CompactionOutcome::Compact:  return "контекст исчерпан";
    }
    return "неизвестно";
}

std::string compaction_user_prompt(const std::string& transcript,
                                   const std::string& prior) {
    std::string prompt = "Вот разговор до сих пор:\n\n<conversation>\n" +
                         transcript + "\n</conversation>\n\n";
    if (!prior.empty()) {
        prompt +=
            "Вот сводка разговора, который шёл ДО разговора выше:\n\n"
            "<prior-summary>\n" + prior + "\n</prior-summary>\n\n"
            "Составь НОВУЮ сводку, объединяющую обе. Старая сводка после "
            "этого отбрасывается: всё, что ты в неё не перенесёшь, будет "
            "потеряно.\n\n"
            "Как объединять:\n"
            "- переноси из <prior-summary> цели, ограничения, решения "
            "пользователя и параллельные работы, даже если в разговоре "
            "выше их не было; отбрасывай только то, что завершено и больше "
            "не нужно;\n"
            "- разговор выше свежее сводки. Там, где они противоречат, "
            "правда — в разговоре: исправь факт и выброси старое "
            "утверждение;\n"
            "- перенеси новое из разговора: сделанное перемести из "
            "«В работе» в «Сделано»;\n"
            "- если препятствие снято, обнови «Заблокировано», оставив "
            "нужное для продолжения;\n"
            "- обнови «Цель» и «Следующий шаг» под текущее состояние.\n\n";
    } else {
        prompt +=
            "Составь новую сводку по разговору в тегах <conversation> "
            "выше, чтобы другой агент-кодировщик мог продолжить работу.\n\n";
    }
    return prompt;
}

namespace {

/* Слово-выход «свести в сводку невозможно». Объявлено в
 * kCompactionSystemPrompt и проверяется здесь же: соглашение, о котором
 * не сказано нигде, не работает нигде. */
bool asks_for_give_up(const std::string& answer) {
    std::string trimmed = answer;
    const size_t first = trimmed.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return false;
    const size_t last = trimmed.find_last_not_of(" \t\r\n");
    trimmed = trimmed.substr(first, last - first + 1);
    for (char& c : trimmed) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return trimmed == "compact";
}

/* Предел длины сводки — по границе строки. Обрезка посреди предложения
 * оставила бы сводку, которая читается как вырванная из контекста;
 * если строки вовсе нет (одна длинная строка) — режем по символу, и флаг
 * truncated всё равно ставится, то есть вызывающий знает, что текст
 * неполон. */
std::string capped_summary(const std::string& summary, bool& truncated) {
    truncated = false;
    if (summary.size() <= static_cast<size_t>(limits::kCompactionSummaryMaxChars)) {
        return summary;
    }
    truncated = true;
    const size_t limit = static_cast<size_t>(limits::kCompactionSummaryMaxChars);
    const size_t nl = summary.rfind('\n', limit);
    if (nl != std::string::npos && nl > 0) return summary.substr(0, nl);
    return text::utf8_prefix(summary, limit);
}

} // namespace

CompactionResult summarize(const std::vector<Message>& history,
                           const SummaryTurn& turn) {
    CompactionResult result;
    if (!turn) {
        result.outcome = CompactionOutcome::Stop;
        result.failure = FailureKind::Provider;
        result.reason = "сводщик недоступен: нет функции запроса";
        return result;
    }
    /* Порядок именно такой: сначала обе половины (разговор И прошлая
     * сводка), потом проверка «есть ли что сжимать». Проверка по одной
     * половине дала бы две разные причины для одного и того же отказа,
     * а отказ виден пользователю именно причиной. */
    const std::string previous = previous_summary(history);
    const std::string transcript = serialize_for_summary(history);
    if (transcript.empty()) {
        /* Сводить нечего: пустая сводка хуже отсутствия сводки, и
         * Continue на пустом разговоре означал бы, что работа продолжается
         * ни на чём.
         *
         * Причины названы раздельно: «пустой разговор» и «после сжатия
         * новых ходов нет» выглядят для пользователя одинаково (сжатия
         * не было), а лечатся по-разному — во втором случае переполнение
         * вернётся на следующем же шаге, и молчание сделало бы его
         * возвращение загадкой. */
        result.outcome = CompactionOutcome::Stop;
        result.reason = previous.empty()
            ? "нечего сжимать: разговор пуст"
            : "нечего сжимать: после прошлого сжатия новых ходов нет";
        return result;
    }

    std::string text;
    std::string error;
    std::vector<ModelMessage> messages;
    messages.push_back({std::string(kRoleUser),
                        compaction_user_prompt(transcript, previous)});
    if (!turn(kCompactionSystemPrompt, messages, text, error)) {
        result.outcome = CompactionOutcome::Stop;
        result.failure = FailureKind::Provider;
        result.reason = "сводщик не ответил: " +
                        (error.empty() ? std::string("без причины") : error);
        return result;
    }

    if (asks_for_give_up(text)) {
        /* Это не сбой провайдера: сеть работает, запрос ушёл. Контекст
         * исчерпан настолько, что места нет даже под сводку. Повторять
         * попытку бессмысленно, поэтому вид сбоя свой, а не Provider. */
        result.outcome = CompactionOutcome::Compact;
        result.failure = FailureKind::ContextOverflow;
        result.reason =
            "сводщик сообщил, что сводка не помещается: контекст исчерпан";
        return result;
    }

    /* Запрет на инструменты здесь МЕХАНИЧЕСКИЙ. Свободного запускателя у
     * сводщика нет, и блок вызова останется в тексте — то есть попадёт в
     * сводку и станет выдумкой, будто инструмент работал. Поэтому вызов
     * не выполняется, а превращается в отказ с именем. */
    std::string rest;
    const std::string call = extract_action(text, rest);
    if (!call.empty()) {
        json::JsonValue args;
        const std::string tool = parse_action(call, args)
                                     ? json::str(args.dump(), "tool")
                                     : std::string();
        result.outcome = CompactionOutcome::Stop;
        result.failure = FailureKind::Tool;
        result.reason = "сводщик вызвал инструмент" +
                        (tool.empty() ? std::string() : " " + tool) +
                        ": у агента-сводщика нет инструментов";
        return result;
    }

    if (rest.find_first_not_of(" \t\r\n") == std::string::npos) {
        result.outcome = CompactionOutcome::Stop;
        result.reason = "сводщик вернул пустой ответ";
        return result;
    }

    result.outcome = CompactionOutcome::Continue;
    result.summary = capped_summary(rest, result.summary_truncated);
    if (result.summary_truncated) {
        result.reason = "сводка обрезана по лимиту длины";
    }
    return result;
}

/* --- И7.8: что история становится после сжатия --- */

std::vector<Message> compacted_history(const Selection& sel,
                                       const std::string& summary,
                                       const CompactionConfig& cfg) {
    /* Сжатие без сводки НЕ применяется, и это не «пустая сводка».
     * Обёртка с пустым текстом заняла бы место головы, а головы нигде
     * больше нет: работа до первого сжатия исчезла бы целиком, и
     * заметить это можно было бы только по тому, что агент перестал
     * помнить, зачем он вообще здесь. Возврат истории целиком означает
     * «сжатия не было»: переполнение вернётся на следующем же шаге, и
     * это уже видно по счётчику, а не по памяти агента.
     *
     * Пустой summary вручную недостижим (summarize() на нём даёт Stop), но
     * функция принимает строку и обязана быть защищена сама: вызывающий
     * — движок, а доверять ему «сюда придут только хорошие значения»
     * здесь нельзя (тот же класс, что с replace_all в 4.3). */
    if (summary.empty()) {
        std::vector<Message> whole(sel.head);
        whole.insert(whole.end(), sel.tail.begin(), sel.tail.end());
        return whole;
    }

    std::vector<Message> out;
    out.reserve(sel.tail.size() + 2);

    /* Обёртка сводки. Часть compaction хранит и сводку, и список
     * свёрнутых сообщений — второй нужен UI («что заменила сводка»), и
     * он выводится ЗДЕСЬ, из головы: перечислять его вручную означало бы
     * второй способ сказать то же самое. */
    Message wrapper = Message::user(std::string());
    std::vector<std::string> replaced;
    replaced.reserve(sel.head.size());
    for (const Message& m : sel.head) replaced.push_back(m.id);
    wrapper.parts.push_back(MessagePart::compaction(summary, std::move(replaced)));
    out.push_back(std::move(wrapper));

    /* Хвост — как есть, целиком: это ровно то, ради чего сжатие вообще
     * делалось, и любая правка здесь означала бы потерю. */
    out.insert(out.end(), sel.tail.begin(), sel.tail.end());

    /* Реплика продолжения — по тому же правилу, что и в порте: только
     * после АВТОсжатия. Ручное сжатие (его появится вместе с 7.10)
     * добавит её вызовом сам, а здесь флаг пользователя «никогда не
     * сжимать» означает ровно то же: никакого автопродолжения. */
    if (cfg.auto_compact) out.push_back(Message::compaction_continue());
    return out;
}

/* --- И7.9: прореживание вывода инструментов --- */

namespace {

/* Вывод, который прореживать нельзя (kPruneProtectedTools). */
bool protected_tool(const std::string& name) {
    for (const char* p : limits::kPruneProtectedTools) {
        if (name == p) return true;
    }
    return false;
}

} // namespace

std::vector<MessagePart*> prune_candidates(std::vector<Message>& history) {
    std::vector<MessagePart*> out;
    /* Счётчик ходов идёт по репликам пользователя: ровно так же, как в
     * порте, где это `msg.info.role === "user"`. Обёртка сводки — тоже
     * пользовательская реплика, но обход всё равно остановится на ней
     * (шаг 5), а начинается он с конца. */
    int turns = 0;
    long long fresh = 0;    /* объём САМЫХ СВЕЖИХ выводов, шаг 1 */
    long long spare = 0;    /* сколько можно очистить, шаг 5 */

    for (size_t mi = history.size(); mi-- > 0; ) {
        Message& m = history[mi];
        if (m.is_user()) ++turns;
        if (turns < 2) continue;
        /* Сводка: всё старше уже выкинуто из истории, и трогать там
         * нечего (шаг 5). Проверка на части, а не на тексте: текст сводки
         * может быть любым. */
        bool is_summary = false;
        for (const MessagePart& q : m.parts) {
            if (q.is(PartKind::Compaction)) is_summary = true;
        }
        if (is_summary) break;

        for (size_t pi = m.parts.size(); pi-- > 0; ) {
            MessagePart& p = m.parts[pi];
            if (!p.is(PartKind::Tool)) continue;
            /* Только завершённые: у работающего вызова вывода ещё нет, а
             * у отказавшего он и так короткий, и метка «очищено» на отказе
             * выглядела бы как поломка инструмента. */
            if (p.state() != ToolState::Completed) continue;
            if (protected_tool(p.tool_name())) continue;
            /* Уже очищенный вызов ПРОПУСКАЕТСЯ, а не обрывает обход.
             *
             * Метка в модели стоит десяток токенов, а не тысячи, поэтому
             * очищенный вывод в счёт защиты не идёт: иначе защита
             * расходовала бы место, которого в контексте уже нет, и
             * прореживание вышло бы дальше, чем нужно.
             *
             * Обрывать обход здесь — правило из порта, и оно опирается на
             * инвариант «за очищенным всё старше очищено». Инвариант
             * держится, но проверка на мутациях показала, что правило
             * неразличимо: обход, проскочивший очищенный вызов, набирает
             * те же токены и предлагает тот же список. При этом
             * ПРОПУСК лучше: сессию могли править руками, и тогда за
             * очищенным может лежать неочищенный гигант, которого
             * прореживание обязано снять, а не пропустить. */
            if (p.output_cleared()) continue;
            const long long size = estimate_tokens(p.output().output);
            fresh += size;
            if (fresh <= limits::kPruneProtectTokens) continue;
            spare += size;
            out.push_back(&p);
        }
    }

    /* Не стоит — не чистим (шаг «а стоит ли»). Список либо весь, либо
     * пуст: частичная очистка выглядела бы как «часть выводов пропала
     * сама», и модель не отличила бы её от поломки. */
    if (spare <= limits::kPruneMinimumTokens) return std::vector<MessagePart*>();
    return out;
}

} // namespace compaction
} // namespace coder
