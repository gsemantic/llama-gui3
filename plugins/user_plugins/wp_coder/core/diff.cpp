/*
 * diff.cpp — И10.5: построчный diff и unified-разметка.
 *
 * Разбор границы файла (BOM/CRLF/завершающий перевод) — через
 * core/text_edit.h, единственное место, которому это принадлежит
 * (правило 9 SESSION_START). Здесь он только читается.
 */

#include "diff.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/limits.h"
#include "text_edit.h"

namespace coder {
namespace diff {

namespace {

/* Ширина окна контекста в unified-патче. Три — то, что печатает git по
 * умолчанию, и то, к чему привык человек: по две строки сверху и снизу
 * плюс сами изменённые. */
constexpr size_t kContext = 3;

std::string join_numbers(size_t start, size_t count) {
    char buf[64];
    if (count == 0) {
        std::snprintf(buf, sizeof(buf), "%zu,0", start ? start - 1 : 0);
    } else if (count == 1) {
        std::snprintf(buf, sizeof(buf), "%zu", start);
    } else {
        std::snprintf(buf, sizeof(buf), "%zu,%zu", start, count);
    }
    return buf;
}

/* Сравнение строк для LCS: точное, без отсечения хвостовых пробелов.
 * Здесь это правильно, а вот в правке (text_edit.h) — нет: там модель
 * присылает окно файла из редакера с визуальными отступами, и «лишний
 * пробел» это не другое состояние. В diff лишний пробел — это правка,
 * которую человек обязан увидеть. */
bool lines_equal(const std::string& a, const std::string& b) {
    return a == b;
}

/* Один hunk: строки уже с номерами и видами. */
std::string render_hunk(const std::vector<DiffLine>& lines, size_t from,
                        size_t to, const char* eol) {
    size_t old_start = 0, new_start = 0, old_count = 0, new_count = 0;
    bool old_seen = false, new_seen = false;
    for (size_t i = from; i < to; ++i) {
        const DiffLine& l = lines[i];
        if (l.kind != LineKind::Added) {
            if (!old_seen) { old_start = l.old_no; old_seen = true; }
            ++old_count;
        }
        if (l.kind != LineKind::Removed) {
            if (!new_seen) { new_start = l.new_no; new_seen = true; }
            ++new_count;
        }
    }
    /* Unified пишет «-l,s» через l = start + s - 1, а для пустого
     * диапазона номер строки на единицу меньше (git так и печатает:
     * добавленный файл — «@@ -0,0 +1 @@»). */
    std::string out = "@@ -" + join_numbers(old_start, old_count) + " +" +
                      join_numbers(new_start, new_count) + " @@";
    for (size_t i = from; i < to; ++i) {
        const DiffLine& l = lines[i];
        const char mark = l.kind == LineKind::Added    ? '+'
                          : l.kind == LineKind::Removed ? '-'
                                                        : ' ';
        out += eol;
        out += mark;
        out += l.text;
    }
    return out;
}

/* Собрать unified из помеченных строк: склеить Context с Added/Removed в
 * hunk'и, отступ в kContext строк по обе стороны. */
std::string render_unified(const std::string& file,
                           const std::vector<DiffLine>& lines,
                           const char* eol, bool no_newline_before,
                           bool no_newline_after) {
    std::string out = "--- a/" + file + eol + "+++ b/" + file;
    size_t i = 0;
    while (i < lines.size()) {
        if (lines[i].kind == LineKind::Context) { ++i; continue; }
        size_t from = i;
        /* Назад на контекст, но не дальше, чем на kContext строк подряд
         * и не за уже закрытый hunk: иначе два соседних изменения
         * слиплись бы в один и человек увидел бы «правку» там, где
         * менялось одно, а второе осталось как есть. */
        size_t back = 0;
        while (back < kContext && from > back &&
               lines[from - back - 1].kind == LineKind::Context) {
            ++back;
        }
        from -= back;
        size_t to = i;
        size_t forward = 0;
        while (to < lines.size()) {
            if (lines[to].kind != LineKind::Context) {
                ++to;
                forward = 0;
                continue;
            }
            if (forward < kContext) { ++to; ++forward; continue; }
            break;
        }
        /* Хвост контекста уже включён в [from, to). */
        out += eol + render_hunk(lines, from, to, eol);
        i = to;
    }
    /* Признак отсутствующего завершающего перевода: git печатает его
     * строкой ПОСЛЕ соответствующей последней строки файла. Наш патч
     * собран по строкам, поэтому ставим в конец — там и находится конец
     * файла. На обеих сторонах сразу он невозможен (тогда файлы разные
     * побайтно и hunk'ов нет), но проверка всё равно явная. */
    if (no_newline_before || no_newline_after) {
        out += eol;
        out += "\\ No newline at end of file";
    }
    return out;
}

/* Разбор имени в кавычках: восьмеричные побеги плюс «обычные».
 * Отказ на незнакомом побеге — вместо догадки. */
std::string unquote_git_name(const std::string& quoted) {
    if (quoted.size() < 2 || quoted.front() != '"' || quoted.back() != '"') {
        return "";
    }
    std::string out;
    for (size_t i = 1; i + 1 < quoted.size(); ++i) {
        const char c = quoted[i];
        if (c != '\\') {
            out += c;
            continue;
        }
        ++i;
        if (i + 1 >= quoted.size()) return "";
        const char e = quoted[i];
        if (e >= '0' && e <= '7') {
            if (i + 2 >= quoted.size()) return "";
            const char e2 = quoted[i + 1];
            const char e3 = quoted[i + 2];
            if (e2 < '0' || e2 > '7' || e3 < '0' || e3 > '7') return "";
            const int value = (e - '0') * 64 + (e2 - '0') * 8 + (e3 - '0');
            out += static_cast<char>(value);
            i += 2;
            continue;
        }
        switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            default: return "";   /* неизвестный побег — отказ */
        }
    }
    return out;
}

} // namespace

bool looks_binary(const std::string& raw) {
    /* Порог 8000 байт — тот, что у git (binary detection читает начало
     * файла), и он назван в шапке: свой порог дал бы свой список
     * «двоичных», и один и тот же файл смотрелся бы то как текст, то
     * как двоичный. */
    const size_t npos = raw.find('\0');
    return npos != std::string::npos && npos < 8000;
}

std::vector<DiffLine> diff_lines(const std::vector<std::string>& before,
                                 const std::vector<std::string>& after,
                                 bool* coarse) {
    if (coarse) *coarse = false;
    std::vector<DiffLine> out;
    size_t p = 0;
    while (p < before.size() && p < after.size() &&
           lines_equal(before[p], after[p])) {
        DiffLine l;
        l.kind = LineKind::Context;
        l.text = before[p];
        l.old_no = p + 1;
        l.new_no = p + 1;
        out.push_back(l);
        ++p;
    }
    size_t suffix = 0;
    while (suffix < before.size() - p && suffix < after.size() - p &&
           lines_equal(before[before.size() - 1 - suffix],
                       after[after.size() - 1 - suffix])) {
        ++suffix;
    }
    const size_t old_mid = before.size() - suffix - p;
    const size_t new_mid = after.size() - suffix - p;

    /* Середина: точное сравнение, если таблица помещается в предел.
     * Иначе — замена целиком, и это сказано в note (make_file_diff). */
    const size_t cells = (old_mid + 1) * (new_mid + 1);
    const bool exact = cells <= limits::kMaxDiffMatrix;
    if (!exact && coarse) *coarse = true;
    std::vector<DiffLine> mid;
    if (old_mid == 0 && new_mid == 0) {
        /* нечего менять */
    } else if (exact) {
        /* LCS таблица: (old_mid+1) x (new_mid+1), плоская. Значение —
         * длина наибольшей общей подпоследовательности суффиксов. */
        std::vector<uint32_t> lcs(cells, 0);
        auto at = [&](size_t i, size_t j) -> uint32_t& {
            return lcs[i * (new_mid + 1) + j];
        };
        for (size_t i = old_mid; i-- > 0;) {
            for (size_t j = new_mid; j-- > 0;) {
                if (lines_equal(before[p + i], after[p + j])) {
                    at(i, j) = at(i + 1, j + 1) + 1;
                } else {
                    at(i, j) = std::max(at(i + 1, j), at(i, j + 1));
                }
            }
        }
        size_t i = 0, j = 0;
        while (i < old_mid && j < new_mid) {
            if (lines_equal(before[p + i], after[p + j])) {
                DiffLine l;
                l.kind = LineKind::Context;
                l.text = before[p + i];
                l.old_no = p + i + 1;
                l.new_no = p + j + 1;
                mid.push_back(l);
                ++i;
                ++j;
            } else if (at(i + 1, j) >= at(i, j + 1)) {
                DiffLine l;
                l.kind = LineKind::Removed;
                l.text = before[p + i];
                l.old_no = p + i + 1;
                mid.push_back(l);
                ++i;
            } else {
                DiffLine l;
                l.kind = LineKind::Added;
                l.text = after[p + j];
                l.new_no = p + j + 1;
                mid.push_back(l);
                ++j;
            }
        }
        while (i < old_mid) {
            DiffLine l;
            l.kind = LineKind::Removed;
            l.text = before[p + i];
            l.old_no = p + i + 1;
            mid.push_back(l);
            ++i;
        }
        while (j < new_mid) {
            DiffLine l;
            l.kind = LineKind::Added;
            l.text = after[p + j];
            l.new_no = p + j + 1;
            mid.push_back(l);
            ++j;
        }
    } else {
        for (size_t i = 0; i < old_mid; ++i) {
            DiffLine l;
            l.kind = LineKind::Removed;
            l.text = before[p + i];
            l.old_no = p + i + 1;
            mid.push_back(l);
        }
        for (size_t j = 0; j < new_mid; ++j) {
            DiffLine l;
            l.kind = LineKind::Added;
            l.text = after[p + j];
            l.new_no = p + j + 1;
            mid.push_back(l);
        }
    }
    out.insert(out.end(), mid.begin(), mid.end());

    for (size_t k = 0; k < suffix; ++k) {
        DiffLine l;
        l.kind = LineKind::Context;
        const size_t oi = before.size() - suffix + k;
        const size_t ni = after.size() - suffix + k;
        l.text = before[oi];
        l.old_no = oi + 1;
        l.new_no = ni + 1;
        out.push_back(l);
    }
    return out;
}

/* Строки ДЛЯ СРАВНЕНИЯ, а не для записи: структура файла взята из
 * text_edit.h, а показ добавлен так, как печатает git, — с `\r` в
 * каждой строке CRLF-файла и с BOM на первой строке. Проверено на живом
 * git 2.39: смена CRLF на LF показывается как изменение КАЖДОЙ строки,
 * а BOM — как изменение первой. Нормализовать их значило бы показать
 * «изменилось две строки» там, где изменились все (шапка core/diff.h,
 * правило 4). */
std::vector<std::string> shown_lines(const TextFile& f) {
    std::vector<std::string> out;
    out.reserve(f.lines.size());
    for (size_t i = 0; i < f.lines.size(); ++i) {
        std::string line = f.lines[i];
        if (f.crlf) line += '\r';
        if (i == 0 && !f.bom.empty()) line = f.bom + line;
        out.push_back(line);
    }
    return out;
}

/* Снятие завершающего перевода строки git показывает как изменение
 * ПОСЛЕДНЕЙ строки: `-b` / `+b` и признак `\ No newline at end of
 * file` (проверено на живом git 2.39). Если последняя строка после
 * сравнения оказалась контекстом, она обязана стать такой парой, иначе
 * патч был бы пустым при непустой разнице байтов, — то есть «правок
 * нет» при правке. */
void mark_last_line_changed(const std::vector<std::string>& before,
                            const std::vector<std::string>& after,
                            std::vector<DiffLine>& lines) {
    if (lines.empty()) return;
    const size_t last = lines.size() - 1;
    if (lines[last].kind != LineKind::Context) return;
    DiffLine removed = lines[last];
    DiffLine added = lines[last];
    removed.kind = LineKind::Removed;
    added.kind = LineKind::Added;
    added.new_no = after.empty() ? 0 : added.new_no;
    lines[last] = removed;
    lines.push_back(added);
    (void)before;
}

FileDiff make_file_diff(const std::string& file, const std::string& before_raw,
                        const std::string& after_raw) {
    FileDiff fd;
    fd.file = file;

    if (before_raw == after_raw) {
        fd.note = "содержимое совпадает";
        return fd;
    }
    if (looks_binary(before_raw) || looks_binary(after_raw)) {
        fd.binary = true;
        fd.note = "двоичный файл: показать нечем, строк нет";
        return fd;
    }

    /* Формат файла — из text_edit.h (правило 9 SESSION_START). */
    const TextFile before = split_text(before_raw);
    const TextFile after = split_text(after_raw);
    const std::vector<std::string> before_lines = shown_lines(before);
    const std::vector<std::string> after_lines = shown_lines(after);

    const bool newline_differs = before.trailing_newline != after.trailing_newline;
    bool coarse = false;
    std::vector<DiffLine> lines =
        diff_lines(before_lines, after_lines, &coarse);
    if (newline_differs) {
        mark_last_line_changed(before_lines, after_lines, lines);
    }

    for (const DiffLine& l : lines) {
        if (l.kind == LineKind::Added) ++fd.additions;
        if (l.kind == LineKind::Removed) ++fd.deletions;
    }
    if (lines.empty() || (fd.additions == 0 && fd.deletions == 0)) {
        /* Байты различаются, а строк — нет. Так бывает, когда файлы
         * различаются ровно завершающим переводом строки И переводами
         * строк внутри (оба случая разобраны на живом git выше): после
         * обязательной правки последней строки сюда попасть не должно,
         * и если попало — это дефект, который обязан быть виден, а не
         * тихо назван «содержимое совпадает». */
        fd.note = "файл отличается только завершающим переводом строки или BOM";
        return fd;
    }

    /* Разделитель строк в патче — всегда `\n`, даже для CRLF-файла:
     * так печатает git, а `\r` остаётся ЧАСТЬЮ строки (её содержимого)
     * и виден в окне как есть. */
    fd.patch = render_unified(file, lines, "\n",
                              !before.trailing_newline, !after.trailing_newline);

    if (coarse) {
        fd.note = "файл изменён сильнее, чем сравнение показывает точно: "
                  "показана замена целиком";
    }
    /* Дальше note может пополниться (обрезка по пределу строк или по
     * размеру). Затирать было бы неверно: у файла, который и сравнён
     * грубо, и обрезан по показам, ВЕРНЫ обе причины, и человек должен
     * знать обе — иначе он примет «показаны первые 400 строк» за
     * «сравнение было точным». */
    auto add_note = [&fd](const std::string& text) {
        if (!fd.note.empty()) fd.note += "; ";
        fd.note += text;
    };

    /* Пределы: сначала строки патча, потом байты. Обрезанный патч
     * остаётся полезным (человек видит начало правки), но обязан
     * сказать, что он не весь. */
    size_t patch_lines = 0;
    for (char c : fd.patch) {
        if (c == '\n') ++patch_lines;
    }
    if (patch_lines > limits::kMaxDiffLinesPerFile) {
        size_t cut = fd.patch.size();
        size_t lines_seen = 0;
        for (size_t i = 0; i < fd.patch.size(); ++i) {
            if (fd.patch[i] != '\n') continue;
            ++lines_seen;
            if (lines_seen > limits::kMaxDiffLinesPerFile) {
                cut = i + 1;
                break;
            }
        }
        fd.patch.resize(cut);
        fd.truncated = true;
        add_note("показаны первые " + std::to_string(limits::kMaxDiffLinesPerFile) +
                 " строк патча");
    }
    if (fd.patch.size() > limits::kMaxDiffBytesPerFile) {
        size_t cut = limits::kMaxDiffBytesPerFile;
        while (cut > 0 && fd.patch[cut - 1] != '\n') --cut;
        fd.patch.resize(cut);
        fd.truncated = true;
        add_note("патч обрезан по размеру (" +
                 std::to_string(limits::kMaxDiffBytesPerFile) + " байт)");
    }
    return fd;
}

std::string parse_git_header_name(const std::string& field) {
    if (field.empty()) return "";
    if (field == "/dev/null") return "";   /* у этой стороны имени нет */
    /* Табуляция в конце — разделитель имени и поля заголовка (git так
     * печатает имя с пробелом). Несколько подряд — тот же разделитель,
     * оставляем только последние пробелы и табуляции. */
    std::string body = field;
    const bool had_tab = !body.empty() && body.back() == '\t';
    while (!body.empty() && (body.back() == '\t' || body.back() == ' ')) {
        body.pop_back();
    }
    if (body.empty()) return "";

    if (body[0] == '"') {
        /* Форма с кавычками: восьмеричные побеги и «обычные». Хвост
         * после закрывающей кавычки — уже не часть имени. */
        const size_t close = body.rfind('"');
        if (close == std::string::npos || close == 0) return "";
        const std::string quoted = body.substr(0, close + 1);
        std::string name = unquote_git_name(quoted);
        if (name.empty()) return "";
        if (name.compare(0, 2, "a/") == 0) return name.substr(2);
        if (name.compare(0, 2, "b/") == 0) return name.substr(2);
        /* Префикс обязателен: без него это не заголовок unified, и
         * возвращать путь целиком значило бы показать diff файла,
         * названного не тем именем. */
        return "";
    }

    if (body.compare(0, 2, "a/") == 0 || body.compare(0, 2, "b/") == 0) {
        const std::string name = body.substr(2);
        if (name.empty()) return "";
        /* Пробел без разделяющей табуляции означал бы, что имя не
         * кончилось: заголовок неоднозначен, и догадываться нельзя. */
        if (!had_tab && name.find(' ') != std::string::npos) {
            return "";
        }
        return name;
    }
    return "";
}

/* --- И11.1: строки показа (границы — в шапке core/diff.h) --- */

namespace {

/* Патч без завершающего перевода: `render_hunk` его не оставляет, и
 * `strip_git_preamble` срезает. Но разбор не должен зависеть от того, чем
 * именно патча владеет вызывающий: завершающий перевод дал бы лишнюю
 * ПУСТУЮ строку, а она внутри hunk'а была бы прочитана как строка файла и
 * сдвинула бы нумерацию. */
std::vector<std::string> split_patch_lines(const std::string& patch) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < patch.size()) {
        const size_t nl = patch.find('\n', pos);
        if (nl == std::string::npos) {
            out.push_back(patch.substr(pos));
            break;
        }
        out.push_back(patch.substr(pos, nl - pos));
        pos = nl + 1;
    }
    if (!out.empty() && out.back().empty() && !patch.empty() &&
        patch.back() == '\n') {
        out.pop_back();
    }
    return out;
}

/* Число из заголовка hunk'а: `@@ -12,7 +13,4 @@`. Возвращает false, если
 * это не заголовок или числа в нём нет. Проверка строгая намеренно: догадка
 * («нумерация с нуля») показала бы в колонке номера чужой номер, а
 * пропуск строки сдвинул бы нумерацию всего hunk'а. */
bool read_number(const std::string& s, size_t* pos, size_t* out) {
    if (*pos >= s.size() || !std::isdigit(static_cast<unsigned char>(s[*pos]))) {
        return false;
    }
    size_t value = 0;
    while (*pos < s.size() && std::isdigit(static_cast<unsigned char>(s[*pos]))) {
        const size_t digit = static_cast<size_t>(s[*pos] - '0');
        if (value > (static_cast<size_t>(1) << 40)) return false;  /* мусор */
        value = value * 10 + digit;
        ++*pos;
    }
    *out = value;
    return true;
}

bool parse_hunk_header(const std::string& line, size_t* old_start,
                       size_t* new_start) {
    if (line.compare(0, 3, "@@ ") != 0 && line.compare(0, 3, "@@-") != 0) {
        return false;
    }
    size_t pos = 3;
    if (pos >= line.size() || line[pos] != '-') return false;
    ++pos;
    if (!read_number(line, &pos, old_start)) return false;
    if (pos < line.size() && line[pos] == ',') {
        ++pos;
        size_t count = 0;
        if (!read_number(line, &pos, &count)) return false;
        (void)count;   /* длину не сверяем: патч бывает обрезан (откл. 137) */
    }
    /* Между диапазонами пробел: unified печатает `@@ -12,7 +13,4 @@`, и без
     * его пропуска заголовок не читался бы ВООБЩЕ — то есть ни одного
     * номера ни в одной строке блока. */
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    if (pos >= line.size() || line[pos] != '+') return false;
    ++pos;
    if (!read_number(line, &pos, new_start)) return false;
    if (pos < line.size() && line[pos] == ',') {
        ++pos;
        size_t count = 0;
        if (!read_number(line, &pos, &count)) return false;
        (void)count;
    }
    return true;
}

bool starts_with(const std::string& s, const char* prefix) {
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}

bool all_whitespace(const std::string& s) {
    for (char c : s) {
        if (c != ' ' && c != '\t' && c != '\r') return false;
    }
    return true;
}

/* Сколько символов отступа в начале строки. */
size_t leading_blanks(const std::string& s) {
    size_t n = 0;
    while (n < s.size() && (s[n] == ' ' || s[n] == '\t')) ++n;
    return n;
}

/* Ширина строки В СИМВОЛАХ, а не в байтах: кириллица в UTF-8 занимает два
 * байта на букву, и по байтам любая русская строка выглядела бы вдвое
 * длиннее, чем она есть. */
size_t char_width(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++n;   /* не продолжение UTF-8-последовательности */
    }
    return n;
}

void pad_left(std::string* out, size_t width) {
    if (char_width(*out) >= width) return;
    std::string pad(width - char_width(*out), ' ');
    *out = pad + *out;
}

} // namespace

std::vector<DiffRow> parse_unified(const std::string& patch) {
    std::vector<DiffRow> out;
    const std::vector<std::string> lines = split_patch_lines(patch);
    bool in_hunk = false;
    bool numbered = false;
    size_t old_no = 0;
    size_t new_no = 0;
    for (const std::string& line : lines) {
        size_t old_start = 0;
        size_t new_start = 0;
        const bool looks_like_header = starts_with(line, "@@ ");
        if (looks_like_header &&
            parse_hunk_header(line, &old_start, &new_start)) {
            DiffRow h;
            h.role = DiffRowRole::Header;
            h.text = line;
            out.push_back(h);
            in_hunk = true;
            numbered = true;
            old_no = old_start;
            new_no = new_start;
            continue;
        }
        if (looks_like_header) {
            /* Заголовок, из которого числа не читаются. Сам он — служебная
             * строка, но тело после него всё равно тело: иначе каждая
             * строка правки попала бы в Meta и колонка номеров у блока
             * пропала бы целиком (граница 3 шапки). Нумерация при этом
             * выключена — номера остаются нулевыми, то есть колонка
             * пустая, а не чужая. */
            DiffRow m;
            m.role = DiffRowRole::Meta;
            m.text = line;
            out.push_back(m);
            in_hunk = true;
            numbered = false;
            old_no = 0;
            new_no = 0;
            continue;
        }
        if (!in_hunk) {
            /* Заголовки файла: `--- a/путь` и `+++ b/путь`. Имя в шапке
             * патча — то же, что виджет печатает над блоком, поэтому в
             * строки показа оно не идёт (граница 2 шапки). */
            if (starts_with(line, "--- ") || starts_with(line, "+++ ")) {
                continue;
            }
            DiffRow m;
            m.role = DiffRowRole::Meta;
            m.text = line;
            out.push_back(m);
            continue;
        }
        DiffRow row;
        /* Пустая строка внутри hunk'а — это контекстная строка пустого
         * содержания: unified печатает её одним пробелом, но некоторые
         * инструменты срезают этот пробел, и прочитать такую строку как
         * служебную значило бы сдвинуть нумерацию всего hunk'а на единицу.
         * Поэтому пустая строка трактуется как признак ' ', а не как
         * «строка без признака». */
        const char mark = line.empty() ? ' ' : line[0];
        /* Текст строки — всё после признака; у пустой строки признака нет
         * физически, и substr(1) на ней бросил бы исключение. */
        const std::string body = line.empty() ? std::string() : line.substr(1);
        switch (mark) {
            case '+':
                row.role = DiffRowRole::Added;
                row.text = body;
                if (numbered) row.new_no = new_no++;
                break;
            case '-':
                row.role = DiffRowRole::Removed;
                row.text = body;
                if (numbered) row.old_no = old_no++;
                break;
            case ' ':
                row.role = DiffRowRole::Context;
                row.text = body;
                if (numbered) {
                    row.old_no = old_no++;
                    row.new_no = new_no++;
                }
                break;
            default:
                /* Всё остальное — служебное: `\ No newline at end of file`
                 * и строка, которую опознать не удалось. Показать её
                 * тусклой честнее, чем выбросить (граница 4 шапки). */
                row.role = DiffRowRole::Meta;
                row.text = line;
                break;
        }
        out.push_back(row);
    }
    return out;
}

size_t trim_diff(std::vector<DiffRow>& rows) {
    bool any = false;
    size_t common = 0;
    for (const DiffRow& r : rows) {
        if (r.role != DiffRowRole::Context && r.role != DiffRowRole::Added &&
            r.role != DiffRowRole::Removed) {
            continue;   /* служебные строки отступом не меряются */
        }
        if (all_whitespace(r.text)) continue;   /* пустая строка не мерит */
        const size_t n = leading_blanks(r.text);
        if (!any || n < common) common = n;
        any = true;
    }
    if (!any || common == 0) return 0;
    for (DiffRow& r : rows) {
        if (r.role != DiffRowRole::Context && r.role != DiffRowRole::Added &&
            r.role != DiffRowRole::Removed) {
            continue;
        }
        /* Срезается не больше, чем есть: у пробельной строки отступ может
         * оказаться короче общего, и съедание «лишнего» съело бы текст. */
        const size_t n = leading_blanks(r.text);
        const size_t cut = n < common ? n : common;
        r.text.erase(0, cut);
    }
    return common;
}

DiffColor diff_role_color(DiffRowRole role) {
    DiffColor c;
    switch (role) {
        case DiffRowRole::Added:
            c.r = 0.42f; c.g = 0.85f; c.b = 0.47f; c.a = 1.0f;
            break;
        case DiffRowRole::Removed:
            c.r = 0.90f; c.g = 0.47f; c.b = 0.45f; c.a = 1.0f;
            break;
        case DiffRowRole::Meta:
            /* Тусклый серый: служебная строка не текст файла, и цветом
             * текста она бы выдавалась за содержимое. */
            c.r = 0.62f; c.g = 0.62f; c.b = 0.68f; c.a = 1.0f;
            break;
        case DiffRowRole::Header:
            c.r = 0.45f; c.g = 0.62f; c.b = 0.90f; c.a = 1.0f;
            break;
        case DiffRowRole::Context:
        default:
            c.r = 0.80f; c.g = 0.80f; c.b = 0.84f; c.a = 1.0f;
            break;
    }
    return c;
}

DiffGutter diff_number_gutter(const std::vector<DiffRow>& rows) {
    size_t max_no = 0;
    for (const DiffRow& r : rows) {
        if (r.old_no > max_no) max_no = r.old_no;
        if (r.new_no > max_no) max_no = r.new_no;
    }
    DiffGutter g;
    size_t digits = 1;
    for (size_t v = max_no; v >= 10; v /= 10) ++digits;
    g.digits = digits < 3 ? 3 : digits;
    /* две колонки номеров, пробел между ними, знак, пробел после знака */
    g.chars = g.digits * 2 + 4;
    return g;
}

size_t diff_widest_row(const std::vector<DiffRow>& rows) {
    size_t widest = 0;
    for (const DiffRow& r : rows) {
        /* Служебные строки ширину содержимого не задают: заголовок hunk'а
         * короткий, а `\ No newline at end of file` — 29 символов, и он
         * растягивал бы окно каждого файла (граница 6 шапки). */
        if (r.role == DiffRowRole::Header || r.role == DiffRowRole::Meta) {
            continue;
        }
        const size_t n = char_width(r.text);
        if (n > widest) widest = n;
    }
    return widest;
}

size_t diff_content_width(const std::vector<DiffRow>& rows,
                          const DiffGutter& gutter) {
    return gutter.chars + diff_widest_row(rows);
}

std::string format_diff_row(const DiffRow& row, const DiffGutter& gutter) {
    std::string number;
    if (row.old_no != 0) number = std::to_string(row.old_no);
    std::string old_col = number;
    pad_left(&old_col, gutter.digits);
    number.clear();
    if (row.new_no != 0) number = std::to_string(row.new_no);
    std::string new_col = number;
    pad_left(&new_col, gutter.digits);

    char mark = ' ';
    if (row.role == DiffRowRole::Added) mark = '+';
    if (row.role == DiffRowRole::Removed) mark = '-';

    std::string text = row.text;
    /* Завершающий CR у CRLF-строки — часть её содержания (отклонение 135),
     * и именно поэтому такая строка и отличается от LF-версии: так
     * считаются изменённые строки. Но в тексте виджета это непечатаемый
     * символ, который ломает раскладку строки, поэтому здесь он
     * снимается: показать его нечем, а потерять из-за него признак правки
     * нельзя. Один, в самом конце: CR внутри строки остаётся. */
    if (!text.empty() && text.back() == '\r') text.pop_back();

    return old_col + " " + new_col + " " + std::string(1, mark) + " " + text;
}

} // namespace diff
} // namespace coder
