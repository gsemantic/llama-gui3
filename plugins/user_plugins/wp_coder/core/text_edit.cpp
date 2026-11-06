#include "text_edit.h"

#include <algorithm>

namespace coder {

namespace {

const char* kBom = "\xEF\xBB\xBF";
const size_t kBomLen = 3;

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
           c == '\v';
}

} // anonymous namespace

std::string trim_end(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && is_space(s[end - 1])) --end;
    return s.substr(0, end);
}

bool same_line(const std::string& a, const std::string& b) {
    if (a == b) return true;
    return trim_end(a) == trim_end(b);
}

TextFile split_text(const std::string& raw) {
    TextFile f;
    size_t start = 0;
    if (raw.size() >= kBomLen && raw.compare(0, kBomLen, kBom) == 0) {
        f.bom = kBom;
        start = kBomLen;
    }
    f.crlf = raw.find("\r\n") != std::string::npos;

    std::string body = raw.substr(start);
    /* Хвостовой перевод строки не создаёт лишней пустой строки: без этого
     * добавление в конец файла давало бы diff в одну пустую строку. */
    f.trailing_newline = !body.empty() && body.back() == '\n';
    if (f.trailing_newline) body.pop_back();

    size_t pos = 0;
    while (pos <= body.size()) {
        size_t nl = body.find('\n', pos);
        if (nl == std::string::npos) {
            /* Последняя строка без завершающего перевода: '\r' убирается
             * ИМЕННО при crlf. С обратным условием (а так было) файл без
             * хвостового перевода получал лишний '\r' при записи. */
            std::string line = body.substr(pos);
            if (f.crlf && !line.empty() && line.back() == '\r') line.pop_back();
            f.lines.push_back(std::move(line));
            break;
        }
        std::string line = body.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        f.lines.push_back(std::move(line));
        pos = nl + 1;
    }
    /* Пустой файл — ноль строк, а не одна пустая. */
    if (f.lines.size() == 1 && f.lines[0].empty() && !f.trailing_newline)
        f.lines.clear();
    return f;
}

std::string join_text(const TextFile& f) {
    std::string out = f.bom;
    const char* nl = f.crlf ? "\r\n" : "\n";
    for (size_t i = 0; i < f.lines.size(); ++i) {
        if (i) out += nl;
        out += f.lines[i];
    }
    if (f.trailing_newline && !f.lines.empty()) out += nl;
    return out;
}


/* ======================================================================
 * И4.3 — каскад заменителей
 * ====================================================================== */

namespace {

/* Тело файла, склеенное '\n', плюс смещения строк. Все стратегии ищут в
 * нём: одни работают построчно, другие — по символам, и две системы
 * координат в одном месте означали бы рассинхрон в отчёте о номере
 * строки. */
struct Body {
    std::string text;
    std::vector<size_t> line_start;
};

Body build_body(const TextFile& f) {
    Body b;
    b.line_start.reserve(f.lines.size());
    for (size_t i = 0; i < f.lines.size(); ++i) {
        b.line_start.push_back(b.text.size());
        b.text += f.lines[i];
        if (i + 1 < f.lines.size()) b.text += '\n';
    }
    return b;
}

std::string body_line(const Body& b, size_t i) {
    const size_t end = (i + 1 < b.line_start.size()) ? b.line_start[i + 1]
                                                     : b.text.size();
    std::string s = b.text.substr(b.line_start[i], end - b.line_start[i]);
    if (!s.empty() && s.back() == '\n') s.pop_back();
    return s;
}

size_t body_line_of(const Body& b, size_t offset) {
    /* line_start отсортирован, поэтому достаточно верхней границы. */
    size_t lo = 0, hi = b.line_start.size();
    while (lo + 1 < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (b.line_start[mid] <= offset) lo = mid;
        else hi = mid;
    }
    return lo;
}

size_t count_occurrences(const std::string& hay, const std::string& needle,
                         size_t from = 0) {
    size_t n = 0;
    size_t p = from;
    while ((p = hay.find(needle, p)) != std::string::npos) {
        ++n;
        p += needle.size();
    }
    return n;
}

/* Нормализованный текст с обратной картой смещений: каждому символу
 * нормализованной строки соответствует диапазон в исходной. Без карты
 * найденное место нельзя перенести обратно, а замена не в том месте —
 * худший из возможных исходов. */
struct Norm {
    std::string text;
    std::vector<size_t> start;
    std::vector<size_t> end;
};

/* Пробелы и переводы строк схлопываются в один пробел. */
Norm normalize_ws(const std::string& s) {
    Norm n;
    size_t i = 0;
    while (i < s.size()) {
        if (is_space(s[i])) {
            const size_t begin = i;
            while (i < s.size() && is_space(s[i])) ++i;
            if (n.text.empty()) continue;  /* ведущие пробелы */
            if (i >= s.size()) break;      /* хвостовые пробелы */
            n.text += ' ';
            n.start.push_back(begin);
            n.end.push_back(i);
        } else {
            n.text += s[i];
            n.start.push_back(i);
            n.end.push_back(i + 1);
            ++i;
        }
    }
    /* Последний пробел мог оказаться хвостовым. */
    if (!n.text.empty() && n.text.back() == ' ') {
        n.text.pop_back();
        n.start.pop_back();
        n.end.pop_back();
    }
    return n;
}

/* Escape-последовательности, которые модель пишет вместо символов. */
Norm normalize_escape(const std::string& s) {
    Norm n;
    auto put = [&](char c, size_t b, size_t e) {
        n.text += c;
        n.start.push_back(b);
        n.end.push_back(e);
    };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            put(s[i], i, i + 1);
            continue;
        }
        const char c = s[i + 1];
        if (c == 'n') put('\n', i, i + 2);
        else if (c == 't') put('\t', i, i + 2);
        else if (c == 'r') put('\r', i, i + 2);
        else if (c == '"') put('"', i, i + 2);
        else if (c == '\'') put('\'', i, i + 2);
        else if (c == '\\') put('\\', i, i + 2);
        else put(s[i], i, i + 1);
        ++i;
    }
    return n;
}

std::string lstrip(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && is_space(s[b])) ++b;
    return s.substr(b);
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && is_space(s[b])) ++b;
    while (e > b && is_space(s[e - 1])) --e;
    return s.substr(b, e - b);
}

size_t levenshtein(const std::string& a, const std::string& b) {
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        prev.swap(cur);
    }
    return prev[b.size()];
}

/* Порог нечёткой стадии из плана (И4.3): и ниже, и выше — свои случаи. */
constexpr double kAnchorThreshold = 0.65;

/* Выше этой длины нечёткое сравнение не идёт. Расстояние Левенштейна
 * квадратично, а файл на 2000 длинных строк (minified-код, base64 в
 * одном файле) превратил бы одну правку в минуты вычислений. Правка
 * такой строки осмысленной не бывает: там нет «похожей написанной»
 * строки, есть данные. */
constexpr size_t kFuzzyMaxLine = 200;

struct Candidate {
    size_t start = 0;
    size_t end = 0;
    /* Регион кратен строкам и включает перевод строки последней строки.
     * Такой регион ОБЯЗАН вернуть перевод, иначе следующая строка
     * склеится с заменой («ONE TWO» + «three» → «ONE TWOthree»). */
    bool whole_lines = false;
};

/* Поиск блока строк с заданным сравнением. Возвращает все места, где
 * блок встречается: решение о единственности принимает вызывающий, иначе
 * «нашлось два раза» и «не нашлось» пришлось бы различать внутри. */
std::vector<Candidate> find_blocks(const Body& b,
                                   const std::vector<std::string>& block,
                                   bool (*eq)(const std::string&,
                                              const std::string&)) {
    std::vector<Candidate> out;
    if (block.empty() || block.size() > b.line_start.size()) return out;
    const size_t last = b.line_start.size() - block.size();
    for (size_t i = 0; i <= last; ++i) {
        bool ok = true;
        for (size_t j = 0; j < block.size(); ++j) {
            if (!eq(body_line(b, i + j), block[j])) { ok = false; break; }
        }
        if (!ok) continue;
        Candidate c;
        c.start = b.line_start[i];
        const size_t after = i + block.size();
        /* line_start[after] — начало следующей строки, то есть region's
         * конец СРАЗУ ПОСЛЕ перевода, завершающего последнюю строку
         * блока: «one\ntwo\n» при after == 2. */
        c.end = (after < b.line_start.size()) ? b.line_start[after]
                                               : b.text.size();
        c.whole_lines = true;
        out.push_back(c);
    }
    return out;
}

bool eq_exact(const std::string& a, const std::string& c) { return a == c; }
bool eq_trimmed(const std::string& a, const std::string& c) {
    return trim_end(a) == trim_end(c);
}
bool eq_unindented(const std::string& a, const std::string& c) {
    return lstrip(a) == lstrip(c);
}

} // anonymous namespace

const char* edit_strategy_name(EditStrategy s) {
    switch (s) {
        case EditStrategy::Simple: return "simple";
        case EditStrategy::LineTrimmed: return "lineTrimmed";
        case EditStrategy::BlockAnchor: return "blockAnchor";
        case EditStrategy::WhitespaceNormalized: return "whitespaceNormalized";
        case EditStrategy::IndentationFlexible: return "indentationFlexible";
        case EditStrategy::EscapeNormalized: return "escapeNormalized";
        case EditStrategy::TrimmedBoundary: return "trimmedBoundary";
        case EditStrategy::ContextAware: return "contextAware";
        case EditStrategy::MultiOccurrence: return "multiOccurrence";
    }
    return "unknown";
}

double line_similarity(const std::string& a, const std::string& b) {
    if (a == b) return 1.0;
    if (a.empty() || b.empty()) return 0.0;
    /* Отсекаем заведомо непохожие строки до расстояния: иначе BlockAnchor
     * стоил бы O(файл × длина²) на каждой правке. Первый символ —
     * главный признак «это вообще та же строка». */
    const size_t la = a.size(), lb = b.size();
    if (la > kFuzzyMaxLine || lb > kFuzzyMaxLine) return 0.0;
    if (la < lb / 2 || lb < la / 2) return 0.0;
    if (a[0] != b[0]) return 0.0;
    const size_t d = levenshtein(a, b);
    const size_t m = std::max(la, lb);
    return 1.0 - static_cast<double>(d) / static_cast<double>(m);
}

EditResult apply_edit(const TextFile& in, const EditRequest& req) {
    EditResult res;
    res.file = in;
    if (req.old_text.empty()) {
        res.error = "пустой текст для поиска (query) — нечего менять";
        return res;
    }

    const Body b = build_body(in);
    const std::vector<std::string> old_lines = split_text(req.old_text).lines;
    const std::string old = req.old_text;

    /* И4.4: непропорциональный спан.
     *
     * В формуле из плана переменные названы неоднозначно
     * (searchLines против oldLines), и подставить их «как есть» нельзя:
     * условие old >= max(old+3, old*2) не выполняется никогда, то есть
     * проверка была бы мёртвой. Смысл единственный и проверяемый —
     * отказать, когда ЗАМЕНЯЕМОГО БЛОКА непропорционально больше, чем
     * нового: это переписывание файла, замаскированное под правку.
     * Обратное (две строки на двести) пропускаем: вставка большого блока
     * законна. */
    if (req.max_span_ratio > 0 && !old_lines.empty()) {
        const size_t removed = old_lines.size();
        const size_t added = split_text(req.new_text).lines.size();
        if (removed >= std::max(added + 3, added * req.max_span_ratio)) {
            res.error = "непропорциональный спан: заменяешь " +
                        std::to_string(removed) + " строк(и) на " +
                        std::to_string(added) +
                        ". Это переписывание файла, а не правка. Пришли только"
                        " нужные строки с контекстом или разбей на несколько"
                        " правок";
            return res;
        }
    }

    /* Место замены: [start, end) в теле файла. */
    size_t start = 0, end = 0;
    bool found = false;
    bool whole_lines = false;
    EditStrategy used = EditStrategy::Simple;

    auto take = [&](const std::vector<Candidate>& cands, EditStrategy st) {
        if (cands.size() != 1) return false;
        used = st;
        start = cands[0].start;
        end = cands[0].end;
        whole_lines = cands[0].whole_lines;
        found = true;
        return true;
    };

    /* 1. Simple — буквальное вхождение. */
    {
        const size_t n = count_occurrences(b.text, old);
        if (n == 1) {
            start = b.text.find(old);
            end = start + old.size();
            found = true;
        } else if (n > 1) {
            res.occurrences = n;
        }
    }

    /* 2. LineTrimmed — разница только в хвостовых пробелах. */
    if (!found && res.occurrences == 0) {
        take(find_blocks(b, old_lines, eq_trimmed), EditStrategy::LineTrimmed);
    }

    /* 3. BlockAnchor — нечёткое совпадение по похожим строкам (>= 0.65).
     * Единственная размытая стадия: и поэтому единственная, где требуется
     * порог, а не «похоже на глаз». */
    if (!found && res.occurrences == 0 && !old_lines.empty()) {
        std::vector<Candidate> cands;
        const size_t last = b.line_start.size() >= old_lines.size()
                                ? b.line_start.size() - old_lines.size() : 0;
        for (size_t i = 0; i <= last && i < b.line_start.size(); ++i) {
            if (line_similarity(body_line(b, i), old_lines.front()) <
                kAnchorThreshold)
                continue;
            bool ok = true;
            for (size_t j = 1; j < old_lines.size(); ++j) {
                if (line_similarity(body_line(b, i + j), old_lines[j]) <
                    kAnchorThreshold) {
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;
            Candidate c;
            c.start = b.line_start[i];
            const size_t after = i + old_lines.size();
            c.end = (after < b.line_start.size()) ? b.line_start[after]
                                                   : b.text.size();
            c.whole_lines = true;
            cands.push_back(c);
        }
        take(cands, EditStrategy::BlockAnchor);
    }

    /* 4. IndentationFlexible — блок тот же, но с другим отступом. Модель
     * присылает блок без отступов постоянно, и это самая частая причина
     * «текст не найден» после точного совпадения. */
    if (!found && res.occurrences == 0) {
        take(find_blocks(b, old_lines, eq_unindented),
             EditStrategy::IndentationFlexible);
    }

    /* 5. TrimmedBoundary — лишние пробелы и переводы по краям фрагмента
     * (артефакт fenced-блока в ответе модели). */
    if (!found && res.occurrences == 0) {
        const std::string t = trim(old);
        if (!t.empty() && t != old) {
            const size_t p = b.text.find(t);
            if (p != std::string::npos && count_occurrences(b.text, t) == 1)
                take({ {p, p + t.size(), false} }, EditStrategy::TrimmedBoundary);
        }
    }

    /* 6. WhitespaceNormalized — любая разница в пробелах и переводах,
     * включая лишнюю пустую строку внутри блока. Широкая стадия: любые
     * две более строгие обязаны идти перед ней, иначе они недостижимы. */
    if (!found && res.occurrences == 0) {
        const Norm nb = normalize_ws(b.text);
        const Norm no = normalize_ws(old);
        if (!no.text.empty()) {
            const size_t p = nb.text.find(no.text);
            /* Именно «одно»: два совпадения после нормализации — это
             * неоднозначность, а не повод взять первое. */
            if (p != std::string::npos && count_occurrences(nb.text, no.text) == 1) {
                const size_t q = p + no.text.size() - 1;
                const size_t s0 = nb.start[p], e0 = nb.end[q];
                /* Заменяем ровно найденный участок, хвостовые пробелы
                 * исходника оставляя на месте. */
                const std::vector<Candidate> one = {
                    {s0, e0}};
                take(one, EditStrategy::WhitespaceNormalized);
            }
        }
    }

    /* 7. EscapeNormalized — модель прислала «\n» вместо перевода строки
     * (или наоборот, файл JSON хранит их как escape). */
    if (!found && res.occurrences == 0) {
        const Norm nb = normalize_escape(b.text);
        const Norm no = normalize_escape(old);
        if (!no.text.empty()) {
            const size_t p = nb.text.find(no.text);
            if (p != std::string::npos && count_occurrences(nb.text, no.text) == 1) {
                const size_t q = p + no.text.size() - 1;
                take({ {nb.start[p], nb.end[q]} },
                     EditStrategy::EscapeNormalized);
            }
        }
    }

    /* 8. ContextAware — блок совпал не целиком: отбрасываем ОДНУ строку
     * контекста — хвостовую или первую. Именно одну: так выглядит
     * «приклеил строку для точности, а она отличается». Отбрасывание
     * нескольких строк означало бы, что блок не найден вовсе, и тогда
     * «совпадение» по одной строке заменило бы нужное на ненужное. */
    if (!found && res.occurrences == 0 && old_lines.size() > 1) {
        const std::vector<std::string> without_last(old_lines.begin(),
                                                    old_lines.end() - 1);
        const std::vector<Candidate> cands = find_blocks(b, without_last, eq_exact);
        if (cands.size() == 1)
            take(cands, EditStrategy::ContextAware);
    }
    if (!found && res.occurrences == 0 && old_lines.size() > 1) {
        const std::vector<std::string> without_first(old_lines.begin() + 1,
                                                     old_lines.end());
        const std::vector<Candidate> cands = find_blocks(b, without_first, eq_exact);
        if (cands.size() == 1)
            take(cands, EditStrategy::ContextAware);
    }

    /* 9. MultiOccurrence — только по явному требованию. */
    if (!found && res.occurrences > 0 && req.replace_all) {
        std::string body = b.text;
        size_t p = 0, n = 0;
        size_t first = std::string::npos;
        while ((p = body.find(old, p)) != std::string::npos) {
            if (first == std::string::npos) first = p;
            body.replace(p, old.size(), req.new_text);
            p += req.new_text.size();
            ++n;
        }
        TextFile nf = split_text(body);
        nf.bom = in.bom;
        nf.crlf = in.crlf;
        nf.trailing_newline = in.trailing_newline;
        res.file = nf;
        res.ok = true;
        res.used = EditStrategy::MultiOccurrence;
        res.occurrences = n;
        res.line = first == std::string::npos ? 0 : body_line_of(b, first) + 1;
        return res;
    }

    if (!found) {
        if (res.occurrences > 1) {
            res.error = "в файле " + std::to_string(res.occurrences) +
                        " одинаковых вхождений. Добавь контекст (строки до и"
                        " после), чтобы было однозначно, или поставь"
                        " replace_all = true, если заменить нужно все.";
        } else {
            res.error = "текст не найден. Ни одна из девяти стадий поиска не"
                        " подошла. Прочитай файл заново (read_file) и пришли"
                        " точный фрагмент.";
        }
        return res;
    }

    /* Замена найденного места в теле и обратная сборка. */
    std::string body = b.text;
    std::string fresh = req.new_text;
    if (whole_lines && end < body.size() && !fresh.empty() &&
        fresh.back() != '\n') {
        /* Регион занял целые строки вместе с их переводами; без этого
         * следующая строка склеилась бы с заменой. */
        fresh += '\n';
    }
    body.replace(start, end - start, fresh);
    TextFile nf = split_text(body);
    nf.bom = in.bom;
    nf.crlf = in.crlf;
    nf.trailing_newline = in.trailing_newline;
    res.file = nf;
    res.ok = true;
    res.used = used;
    res.occurrences = 1;
    res.line = body_line_of(b, start) + 1;
    return res;
}

} // namespace coder
