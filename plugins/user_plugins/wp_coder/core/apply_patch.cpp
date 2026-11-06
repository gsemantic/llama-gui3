#include "apply_patch.h"

#include <algorithm>

namespace coder {
namespace patch {

namespace {

const char* kBegin = "*** Begin Patch";
const char* kEnd = "*** End Patch";
const char* kEndOfFile = "*** End of File";

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

/* Разобрать текст патча на строки.
 *
 * Своя функция, а не split_text: patch — не файл. BOM и CRLF здесь нужно
 * снять (модель пишет текст сообщения), а хвостовые пробелы — сохранить:
 * это часть добавляемой строки, и молчаливая обрезка была бы изменением
 * данных, которое никто не просил. */
std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::string body = text;
    if (body.size() >= 3 && body.compare(0, 3, "\xEF\xBB\xBF") == 0)
        body = body.substr(3);
    std::string cur;
    for (size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            out.push_back(cur);
            cur.clear();
        } else if (c == '\r' && i + 1 < body.size() && body[i + 1] == '\n') {
            continue;
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool is_directive(const std::string& t) {
    return t.rfind("***", 0) == 0;
}

std::string after_prefix(const std::string& t, const std::string& prefix) {
    if (t.rfind(prefix, 0) != 0) return "";
    return trim(t.substr(prefix.size()));
}

/* Найти блок old_lines в lines начиная с from. Сначала точное сравнение,
 * затем — с игнром хвостовых пробелов. Только два уровня: «похожий»
 * третий уровень превратил бы правку не туда, а патч должен либо
 * примениться, либо честно сказать, что файл не тот. */
size_t find_block(const std::vector<std::string>& lines,
                  const std::vector<std::string>& block, size_t from,
                  bool relaxed) {
    if (block.empty() || block.size() > lines.size()) return std::string::npos;
    for (size_t i = from; i + block.size() <= lines.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < block.size(); ++j) {
            const bool same = relaxed ? same_line(lines[i + j], block[j])
                                      : lines[i + j] == block[j];
            if (!same) { ok = false; break; }
        }
        if (ok) return i;
    }
    return std::string::npos;
}

/* Первая строка, содержащая подсказку контекста. */
size_t find_context(const std::vector<std::string>& lines,
                    const std::string& needle, size_t from) {
    for (size_t i = from; i < lines.size(); ++i) {
        if (lines[i].find(needle) != std::string::npos) return i;
    }
    return std::string::npos;
}

std::string first_line_of(const std::vector<std::string>& v) {
    if (v.empty()) return "";
    std::string s = v.front();
    if (s.size() > 60) s = s.substr(0, 57) + "...";
    return s;
}

} // anonymous namespace

Parsed parse(const std::string& text) {
    Parsed out;
    const std::vector<std::string> lines = split_lines(text);

    size_t i = 0;
    while (i < lines.size() && trim(lines[i]).empty()) ++i;
    if (i == lines.size() || trim(lines[i]) != kBegin) {
        out.error = "патч должен начинаться со строки '" + std::string(kBegin) +
                    "' (получено: '" + first_line_of(lines) + "')";
        return out;
    }
    ++i;

    FilePatch file;
    bool have_file = false;
    Hunk hunk;
    bool have_hunk = false;
    bool done = false;

    /* Закрыть текущий хук в текущий файл. */
    auto close_hunk = [&]() -> bool {
        if (!have_hunk) return true;
        if (file.hunks.empty() && hunk.old_lines.empty() &&
            hunk.new_lines.empty()) {
            /* пустой @@ без содержимого — не ошибка, но и не хук */
            have_hunk = false;
            return true;
        }
        file.hunks.push_back(hunk);
        hunk = Hunk{};
        have_hunk = false;
        return true;
    };
    auto close_file = [&]() {
        if (!have_file) return;
        close_hunk();
        out.files.push_back(file);
        file = FilePatch{};
        have_file = false;
    };

    for (; i < lines.size() && !done; ++i) {
        const std::string raw = lines[i];
        const std::string t = trim(raw);

        if (is_directive(t)) {
            if (t == kEnd) {
                close_file();
                done = true;
                /* После End Patch只能是 пустые строки: недописанный хвост
                 * патча выглядел бы как успешное применение. */
                for (size_t j = i + 1; j < lines.size(); ++j) {
                    if (!trim(lines[j]).empty()) {
                        out.files.clear();
                        out.error = "после '" + std::string(kEnd) +
                                    "' остался текст: '" + first_line_of(lines) +
                                    "'. Патч не применён целиком.";
                        return out;
                    }
                }
                break;
            }
            if (t == kEndOfFile) {
                if (!have_hunk) {
                    out.error = "'" + std::string(kEndOfFile) +
                                "' стоит вне хука (нужна строка '@@')";
                    return out;
                }
                hunk.is_end_of_file = true;
                continue;
            }
            /* 'Move to' относится к текущему файлу, поэтому разбирается
             * ДО закрытия секции — иначе файл закрылся бы и директива
             * осталась бы без адресата. */
            const std::string mv = after_prefix(t, "*** Move to:");
            if (!mv.empty()) {
                if (have_file && file.op == Op::Update) {
                    file.move_to = mv;
                } else {
                    out.error = "'*** Move to:' допустим только сразу после"
                                " '*** Update File:'";
                    return out;
                }
                continue;
            }
            close_file();
            const std::string add = after_prefix(t, "*** Add File:");
            const std::string upd = after_prefix(t, "*** Update File:");
            const std::string del = after_prefix(t, "*** Delete File:");
            if (!add.empty()) {
                file = FilePatch{};
                file.op = Op::Add;
                file.path = add;
                have_file = true;
            } else if (!upd.empty()) {
                file = FilePatch{};
                file.op = Op::Update;
                file.path = upd;
                have_file = true;
            } else if (!del.empty()) {
                file = FilePatch{};
                file.op = Op::Delete;
                file.path = del;
                have_file = true;
            } else {
                out.error = "неизвестная директива: '" + t + "'. Ожидались"
                            " Add File / Update File / Delete File / Move to /"
                            " End of File / End Patch";
                return out;
            }
            continue;
        }

        if (!have_file) {
            out.error = "строка '" + first_line_of({raw}) +
                        "' вне секции файла: сначала '*** Add File:',"
                        " '*** Update File:' или '*** Delete File:'";
            return out;
        }

        if (!t.empty() && t[0] == '@' && t[1] == '@') {
            close_hunk();
            hunk = Hunk{};
            hunk.change_context = trim(t.substr(2));
            have_hunk = true;
            continue;
        }

        /* Строка содержимого. */
        if (!have_hunk) {
            /* Хук без '@@' принимаем: у локальных моделей строка с
             * двумя '@@' выпадает чаще, чем формат без неё. Ошибка всё
             * равно вскроется при поиске old_lines в файле. */
            hunk = Hunk{};
            have_hunk = true;
        }

        if (file.op == Op::Add) {
            if (raw.empty() || raw[0] != '+') {
                out.error = "в '*** Add File:' каждая строка должна начинаться"
                            " с '+' (получено: '" + first_line_of({raw}) + "')";
                return out;
            }
            hunk.new_lines.push_back(raw.substr(1));
            continue;
        }
        if (file.op == Op::Delete) {
            out.error = "'*** Delete File:' не принимает содержимое"
                        " (получено: '" + first_line_of({raw}) + "')";
            return out;
        }

        if (raw.empty()) {
            hunk.old_lines.push_back("");
            hunk.new_lines.push_back("");
        } else if (raw[0] == '+') {
            hunk.new_lines.push_back(raw.substr(1));
        } else if (raw[0] == '-') {
            hunk.old_lines.push_back(raw.substr(1));
        } else if (raw[0] == ' ') {
            hunk.old_lines.push_back(raw.substr(1));
            hunk.new_lines.push_back(raw.substr(1));
        } else {
            out.error = "строка хука '" + first_line_of({raw}) +
                        "' не начинается ни с '+', ни с '-', ни с ' '"
                        " (патч: контекст с пробела, добавление с '+',"
                        " удаление с '-')";
            return out;
        }
    }

    if (!done) {
        out.error = "патч оборван: нет строки '" + std::string(kEnd) + "'";
        return out;
    }
    if (out.files.empty()) {
        out.error = "в патче нет ни одной операции";
        return out;
    }
    return out;
}

Applied apply_hunks(const TextFile& in, const std::vector<Hunk>& hunks) {
    Applied res;
    res.file = in;
    if (hunks.empty()) {
        res.error = "у '*** Update File:' нет ни одного хука '@@'";
        return res;
    }

    size_t cursor = 0;
    for (size_t hi = 0; hi < hunks.size(); ++hi) {
        const Hunk& h = hunks[hi];
        res.failed_hunk = hi;

        if (h.old_lines.empty()) {
            if (!h.is_end_of_file) {
                res.error = "нечего искать: хук без строк контекста."
                            " Чтобы дописать в конец файла, добавь"
                            " '*** End of File'";
                return res;
            }
            res.file.lines.insert(res.file.lines.end(), h.new_lines.begin(),
                                  h.new_lines.end());
            res.file.trailing_newline = true;
            continue;
        }

        /* Подсказка контекста сужает поиск до места после неё — иначе
         * второй одинаковый кусок в файле получил бы не ту правку. */
        size_t from = cursor;
        if (!h.change_context.empty()) {
            const size_t at = find_context(res.file.lines, h.change_context, cursor);
            if (at != std::string::npos) from = at + 1;
        }

        size_t at = std::string::npos;
        if (h.is_end_of_file) {
            const size_t n = res.file.lines.size();
            if (h.old_lines.size() <= n) {
                const size_t pos = n - h.old_lines.size();
                bool ok = true;
                for (size_t j = 0; j < h.old_lines.size(); ++j) {
                    if (!same_line(res.file.lines[pos + j], h.old_lines[j])) {
                        ok = false;
                        break;
                    }
                }
                if (ok) at = pos;
            }
        } else {
            at = find_block(res.file.lines, h.old_lines, from, false);
            if (at == std::string::npos)
                at = find_block(res.file.lines, h.old_lines, from, true);
            /* Подсказка контекста сужает, но не запрещает: файл мог
             * измениться и после неё. */
            if (at == std::string::npos && from != 0)
                at = find_block(res.file.lines, h.old_lines, 0, false);
            if (at == std::string::npos)
                at = find_block(res.file.lines, h.old_lines, 0, true);
        }

        if (at == std::string::npos) {
            res.error = "не найден блок из " + std::to_string(h.old_lines.size()) +
                        " строк(ы), начинающийся с '" + first_line_of(h.old_lines) +
                        "'" +
                        (h.change_context.empty()
                             ? ""
                             : " (рядом с контекстом '" + h.change_context + "')") +
                        (h.is_end_of_file ? " [ожидался конец файла]" : "") +
                        ". Файл изменился? Прочитай его заново и пришли"
                        " патч с точным текстом.";
            return res;
        }

        res.file.lines.erase(res.file.lines.begin() + at,
                             res.file.lines.begin() + at + h.old_lines.size());
        res.file.lines.insert(res.file.lines.begin() + at, h.new_lines.begin(),
                              h.new_lines.end());
        cursor = at + h.new_lines.size();
    }

    res.ok = true;
    return res;
}

} // namespace patch
} // namespace coder
