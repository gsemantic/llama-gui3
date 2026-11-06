#include "token_panel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace coder {
namespace token_panel {

namespace {

/* Неразрывный пробел: число «12 345» не должно разрываться переносом на
 * «12» и «345» — это читается как два числа. */
const char* const kNbsp = "\xC2\xA0";

/* Ставка задана, если она положительна. Ноль — «человек не задал» (п. 2
 * шапки), отрицательное значение — опечатка, и трактовать его как цену
 * было бы хуже, чем трактовать как «не задано». */
bool price_known(double price) { return price > 0.0; }

/* Деньги печатаются так, чтобы ненулевая сумма не выглядела нулевой.
 * «$0.0000» при сумме 3e-5 читается как «бесплатно» — ровно то
 * заключение, которого панель обязана не допускать (п. 2 шапки).
 * Знаков столько, сколько нужно величине: у чатов суммы копеечные, у
 * пакетных прогонов — десятки долларов, и «$0.0031» полезнее «$0.00». */
std::string cost_text(double usd) {
    if (usd <= 0.0) return "$0.00";
    if (usd < 0.0001) return "< $0.0001";
    if (usd < 1.0) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "$%.4f", usd);
        return buf;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "$%.2f", usd);
    return buf;
}

std::string seconds_text(double seconds) {
    if (seconds <= 0.0) return "—";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f с", seconds);
    return buf;
}

std::string speed_text(double tps) {
    if (tps <= 0.0) return "—";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f ток/с", tps);
    return buf;
}

std::string share_text(const Snapshot& snap) {
    if (snap.context_room <= 0) return std::string();
    const double share =
        100.0 * static_cast<double>(snap.context.tokens) /
        static_cast<double>(snap.context_room);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f%%", share);
    return buf;
}

}  // namespace

std::string format_count(long long value) {
    const bool negative = value < 0;
    /* Модуль берётся БЕЗЗНАКОВЫМ: `-value` для самого маленького long long
     * переполняется, и панель напечатала бы мусор ровно на граничном
     * значении — то есть на том, которое и приходит от битого провайдера. */
    unsigned long long v = negative
        ? 0ULL - static_cast<unsigned long long>(value)
        : static_cast<unsigned long long>(value);
    const std::string digits = std::to_string(v);
    /* Группы по три справа налево: разделитель ставится перед каждой
     * группой, кроме первой, а первая группа короче трёх цифр. */
    std::string out;
    const std::size_t first = digits.size() % 3 == 0 ? 3 : digits.size() % 3;
    out += digits.substr(0, first);
    for (std::size_t i = first; i < digits.size(); i += 3) {
        out += kNbsp;
        out += digits.substr(i, 3);
    }
    return negative ? "-" + out : out;
}

double cost_for(const Snapshot& snap) {
    if (!price_known(snap.input_price_per_1m) ||
        !price_known(snap.output_price_per_1m)) {
        return 0.0;
    }
    const double in = static_cast<double>(snap.input) / 1e6 * snap.input_price_per_1m;
    const double out = static_cast<double>(snap.output) / 1e6 * snap.output_price_per_1m;
    return in + out;
}

std::string cost_line(const Snapshot& snap) {
    const bool in_known = price_known(snap.input_price_per_1m);
    const bool out_known = price_known(snap.output_price_per_1m);
    if (!in_known && !out_known) {
        return "стоимость: цена не задана — "
               "wp_coder.price_input_per_1m, wp_coder.price_output_per_1m";
    }
    if (!in_known) {
        return "стоимость: не посчитана, не задана цена входа "
               "(wp_coder.price_input_per_1m)";
    }
    if (!out_known) {
        return "стоимость: не посчитана, не задана цена выхода "
               "(wp_coder.price_output_per_1m)";
    }
    /* Кэш тарифицируется по цене входа и об этом сказано: у части
     * провайдеров он дешевле, и сумма выше — верхняя оценка (п. 4). */
    const std::string cache_note =
        (snap.cache_read > 0 || snap.cache_write > 0)
            ? ", кэш по цене входа — верхняя оценка"
            : "";
    return "стоимость: " + cost_text(cost_for(snap)) + cache_note;
}

std::string speed_line(const Snapshot& snap) {
    if (snap.tokens_estimated) {
        /* Оценка chars/4 не показывается числом: рядом с «измерено» для
         * контекста она читалась бы как измерение (п. 5 шапки). */
        return "скорость: оценка по длине ответа, провайдер не прислал usage";
    }
    if (snap.tokens_per_second > 0.0) {
        return "скорость: " + speed_text(snap.tokens_per_second) +
               " за " + seconds_text(snap.llm_seconds) + " у модели";
    }
    return "скорость: нет данных — ни одного запроса с usage";
}

std::string context_line(const Snapshot& snap, bool* tight) {
    if (tight) *tight = false;
    std::string out = "контекст: " + format_count(snap.context.tokens);
    if (snap.context_room > 0) {
        out += " / " + format_count(snap.context_room);
        const std::string share = share_text(snap);
        if (!share.empty()) out += " (" + share + ")";
        if (tight) {
            *tight = snap.context.tokens >= snap.context_room;
        }
    } else {
        /* Порог неизвестен — словами, а не нулём: ноль процентов выглядел
         * бы как «контекст пуст» (п. 6 шапки). */
        out += " (лимит окна не задан: wp_coder.context_limit";
    }
    out += std::string(", ") + compaction::token_source_name(snap.context.source);
    return out;
}

Panel build(const Snapshot& snap) {
    Panel p;
    /* Видимость: панель появляется, когда задача что-то измерила или
     * потратила. Ноль всего — «задача не идёт» (п. 7 шапки). */
    const bool anything = snap.input > 0 || snap.output > 0 || snap.steps > 0 ||
                          snap.response_seconds > 0.0 || snap.llm_seconds > 0.0 ||
                          snap.context.tokens > 0;
    p.visible = anything;
    if (!p.visible) return p;

    p.input_line = "вход: " + format_count(snap.input);
    if (snap.cache_read > 0 || snap.cache_write > 0) {
        p.input_line += " (кэш: чтение " + format_count(snap.cache_read) +
                        ", запись " + format_count(snap.cache_write) + ")";
    }

    p.output_line = "выход: " + format_count(snap.output);
    if (snap.reasoning > 0) {
        p.output_line += " (размышление: " + format_count(snap.reasoning) + ")";
    }

    p.cost_line = cost_line(snap);
    p.speed_line = speed_line(snap);

    bool tight = false;
    p.context_line = context_line(snap, &tight);
    p.context_tight = tight;

    p.steps_line = "шагов: " + std::to_string(snap.steps) +
                   ", время задачи " + seconds_text(snap.response_seconds) +
                   ", у модели " + seconds_text(snap.llm_seconds);
    return p;
}

}  // namespace token_panel
}  // namespace coder