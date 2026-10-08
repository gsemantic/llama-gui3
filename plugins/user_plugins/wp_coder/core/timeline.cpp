// timeline.cpp — И11.3: дерево вызовов инструментов по ходам.

#include "timeline.h"

#include "json_utils.h"

#include <cstdio>

namespace coder {
namespace timeline {
namespace {

/* Статус вызова ПО-ЧЕЛОВЕЧЕСКИ. Это НЕ `tool_state_name()`: то имя —
 * протокол файла сессии («completed», «error»), и подпись в окне на нём
 * читалась бы как отладочная строка. Протокол и подпись — два разных
 * назначения одного и того же состояния, а не две копии состояния. */
const char* status_label(ToolState status) {
    switch (status) {
        case ToolState::Pending:   return "не начат";
        case ToolState::Running:   return "работает";
        case ToolState::Completed: return "готов";
        case ToolState::Error:     return "ошибка";
    }
    return "неизвестно";
}

/* Реплика человека, встреченная по дороге: идентификатор сообщения,
 * первая строка её текста и признак обрезки. */
struct ReplyLine {
    std::string id;
    std::string text;
    bool clipped = false;
};

/* Сумма времён: -1, пока не измерено НИ ОДНОГО вызова, чтобы «нет
 * времени» и «ноль секунд» не совпали в подписи. */
void add_duration(long long* sum, long long ms) {
    if (ms < 0) return;
    *sum = (*sum < 0 ? 0 : *sum) + ms;
}

} // namespace

const char* turn_status_label(TurnStatus status) {
    switch (status) {
        case TurnStatus::Text:    return "только текст";
        case TurnStatus::Working: return "работает";
        case TurnStatus::Failed:  return "с ошибкой";
        case TurnStatus::Done:    return "готов";
    }
    return "неизвестно";
}

Source timeline_source(const std::vector<Message>& history,
                       size_t max_turns) {
    /* Один обход назад, а не два: номера ходов считаются по ВСЕЙ истории
     * (иначе показ последних тридцати переименовал бы тридцатый ход в
     * «Ход 1»), а якорь-реплика ищется среди уже пройденных сообщений. */
    size_t total_turns = 0;
    for (const Message& m : history) {
        if (m.is_assistant()) ++total_turns;
    }

    Source out;
    if (max_turns == 0) {
        out.omitted_turns = total_turns;
        return out;
    }

    /* Реплики, встреченные по дороге. Держим ВСЕ встреченные, а не только
     * последнюю: у хода один родитель на несколько ходов подряд, и
     * «последняя реплика» покрыла бы якорь только для самого нового хода,
     * а у остальных он был бы пустым. */
    std::vector<ReplyLine> replies;

    for (size_t i = history.size(); i-- > 0;) {
        const Message& m = history[i];
        if (m.is_user()) {
            std::string full = m.text();
            const size_t nl = full.find('\n');
            if (nl != std::string::npos) full = full.substr(0, nl);
            /* Обрез по СИМВОЛАМ, а не по байтам: кириллица занимает два
             * байта на букку, и `substr(0, 60)` разрезал бы русскую
             * реплику пополам — в подписи дерева появился бы обрезанный
             * байт (то же основание, что у ширины строки diff, отклонение
             * 146). «Обрезано» берётся из САМОГО обреза, а не из сравнения
             * длин: починенный битый байт занимает три байта вместо
             * одного, и сравнение дало бы «не обрезано» ровно на сломанном
             * тексте. */
            bool truncated = false;
            ReplyLine line;
            line.id = m.id;
            /* Текст кладутся ЧИСТЫМ (обрезанным, но без многоточия), а знак
             * «обрезано» — отдельным признаком: подпись собирает его сама,
             * и приём с приклеенным многоточием внутри данных означал бы,
             * что любое другое место показа (11.4) не сможет отличить
             * обрезанную реплику от целой, написавшей «…» сама. */
            line.text = text::utf8_prefix_chars(
                full, limits::kMaxTimelineReplyChars, &truncated);
            line.clipped = truncated;
            replies.push_back(std::move(line));
            continue;
        }
        if (!m.is_assistant()) continue;

        TurnSource t;
        t.number = total_turns;
        t.id = m.id;
        t.parent_id = m.parent_id;
        for (const MessagePart& p : m.parts) {
            if (!p.is(PartKind::Tool)) continue;
            CallSource c;
            c.tool = p.tool_name();
            c.call_id = p.call_id();
            c.status = p.state();
            c.duration_ms = p.duration_ms();
            t.calls.push_back(std::move(c));
        }
        out.turns.push_back(std::move(t));
        --total_turns;
        if (out.turns.size() >= max_turns) break;
    }

    /* Якорь ищем после обхода: к моменту, как ход найден, его родитель уже
     * пройден (родитель всегда раньше ребёнка и по порядку в истории, и по
     * идентификатору — счётчик монотонен, И5.5). */
    for (TurnSource& t : out.turns) {
        if (t.parent_id.empty()) continue;
        for (const ReplyLine& r : replies) {
            if (r.id != t.parent_id) continue;
            t.reply_to = r.text;
            t.reply_clipped = r.clipped;
            /* НАЙДЕНА, а не «родитель непустой»: реплика, которой в
             * сессии нет, и непустой parent_id — разные вещи, и подпись
             * обязана их различать (после компакшна И7.7 второй случай
             * выглядел бы как «реплика есть, но пустая»). */
            t.reply_found = true;
            break;
        }
    }
    std::reverse(out.turns.begin(), out.turns.end());
    /* Пропущенные считаются по счётчику, а не как «взято минус показано»:
     * второй способ дал бы ноль всегда. */
    out.omitted_turns = total_turns;
    return out;
}

Timeline build_timeline(const Source& source) {
    Timeline out;
    out.omitted_turns = source.omitted_turns;

    for (const TurnSource& src : source.turns) {
        Turn t;
        t.number = src.number;
        t.id = src.id;
        t.reply_to = src.reply_to;
        t.reply_clipped = src.reply_clipped;
        t.reply_known = src.reply_found;
        bool open_call = false;
        bool failed = false;
        for (const CallSource& c : src.calls) {
            Call call;
            call.tool = c.tool;
            call.call_id = c.call_id;
            call.status = c.status;
            call.duration_ms = c.duration_ms;
            t.calls.push_back(std::move(call));
            if (c.status == ToolState::Pending || c.status == ToolState::Running) {
                open_call = true;
            }
            if (c.status == ToolState::Error) failed = true;
            if (c.duration_ms < 0) {
                ++t.untimed_calls;
                ++out.untimed_calls;
            } else {
                add_duration(&t.duration_ms, c.duration_ms);
            }
        }
        /* Порядок выбора назван в шапке: работающий вызов важнее отказа. */
        if (t.calls.empty()) {
            t.status = TurnStatus::Text;
        } else if (open_call) {
            t.status = TurnStatus::Working;
        } else if (failed) {
            t.status = TurnStatus::Failed;
        } else {
            t.status = TurnStatus::Done;
        }
        add_duration(&out.duration_ms, t.duration_ms);
        out.calls_total += t.calls.size();
        out.turns.push_back(std::move(t));
    }
    /* Открыт самый новый показанный ход: открытые тридцать деревьев окно
     * не показывают, а закрытый последний ход прячет ровно то, ради чего
     * человек смотрит на дерево. */
    if (!out.turns.empty()) out.turns.back().default_open = true;
    return out;
}

std::string duration_text(long long ms) {
    if (ms < 0) return "—";
    if (ms < 1000) return std::to_string(ms) + " мс";
    if (ms < 60 * 1000) {
        /* Целые, без плавающей точки: одна и та же длительность обязана
         * печататься одинаково и в окне, и в проверке. */
        const long long sec = ms / 1000;
        const long long tenth = (ms % 1000) / 100;
        return std::to_string(sec) + "." + std::to_string(tenth) + " с";
    }
    if (ms < 60 * 60 * 1000) {
        const long long min = ms / (60 * 1000);
        const long long sec = (ms % (60 * 1000)) / 1000;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%lld мин %02lld с", min, sec);
        return buf;
    }
    const long long hour = ms / (60 * 60 * 1000);
    const long long min = (ms % (60 * 60 * 1000)) / (60 * 1000);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%lld ч %02lld мин", hour, min);
    return buf;
}

std::string duration_with_gaps(long long ms, size_t untimed) {
    std::string out = duration_text(ms);
    if (untimed > 0) {
        out += " + " + std::to_string(untimed) + " без времени";
    }
    return out;
}

std::string call_label(const Call& call) {
    return call.tool + ": " + status_label(call.status) + ", " +
           duration_text(call.duration_ms);
}

std::string turn_label(const Turn& turn) {
    std::string out = "Ход " + std::to_string(turn.number) + ": ";
    if (turn.calls.empty()) {
        return out + std::string(turn_status_label(turn.status)) + ", вызовов нет";
    }
    out += std::string(turn_status_label(turn.status));
    out += ", вызовов: " + std::to_string(turn.calls.size());
    out += ", время вызовов: " +
           duration_with_gaps(turn.duration_ms, turn.untimed_calls);
    /* Отсутствие якоря НАЗЫВАЕТСЯ: после компакшна (И7.7) родительская
     * реплика в сессии отсутствует, и пустой якорь читался бы как «ход без
     * запроса», то есть как несуществующий ход. */
    if (!turn.reply_known) {
        return out + " (реплика человека не найдена)";
    }
    if (turn.reply_to.empty()) return out + " — (реплика человека пуста)";
    return out + " — " + turn.reply_to +
           (turn.reply_clipped ? "\xE2\x80\xA6" : "");
}

std::string timeline_head(const Timeline& timeline) {
    return "Таймлайн: ходов " + std::to_string(timeline.turns.size()) +
           ", вызовов " + std::to_string(timeline.calls_total) +
           ", время вызовов: " +
           duration_with_gaps(timeline.duration_ms, timeline.untimed_calls);
}

StatusColor status_color(ToolState status) {
    StatusColor c;
    switch (status) {
        case ToolState::Pending:
            c.r = 0.62f; c.g = 0.62f; c.b = 0.68f;
            break;
        case ToolState::Running:
            c.r = 0.90f; c.g = 0.75f; c.b = 0.35f;
            break;
        case ToolState::Completed:
            c.r = 0.55f; c.g = 0.85f; c.b = 0.55f;
            break;
        case ToolState::Error:
            c.r = 0.90f; c.g = 0.47f; c.b = 0.45f;
            break;
    }
    return c;
}

} // namespace timeline
} // namespace coder