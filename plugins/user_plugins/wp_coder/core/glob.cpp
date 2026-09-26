#include "glob.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

namespace coder {
namespace fileglob {

namespace {

/* Сопоставить один сегмент пути с шаблоном сегмента: только * и ?.
 * Итеративно с откатом: у отката индекс последней '*', поэтому
 * худший случай линеен, а не экспоненциальный. */
bool match_segment(const std::string& pat, const std::string& txt) {
    size_t p = 0, t = 0;
    size_t star = std::string::npos;  /* позиция последней '*' в шаблоне */
    size_t mark = 0;                   /* сколько символов она съела */
    while (t < txt.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == txt[t])) {
            ++p;
            ++t;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string::npos) {
            /* Не подошло — отдаём '*' один символ текста и пробуем снова. */
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

std::vector<std::string> split_segments(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '/') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

/* Раскрыть {a,b} в набор шаблонов. Рекурсия по вложенным скобкам, как в
 * bash: {a,b} -> a, b;  {src,tests}/x -> src/x, tests/x.
 *
 * Скобки без запятой — литерал ({a} в bash так и остаётся {a}), но только
 * если внутри нет вложенных скобок: иначе {a{b,c}} схлопнулся бы в себя и
 * тихо перестал бы что-либо раскрывать. */
void expand_braces(const std::string& pat, std::vector<std::string>& out) {
    size_t open = pat.find('{');
    if (open == std::string::npos) {
        out.push_back(pat);
        return;
    }
    size_t close = std::string::npos;
    int depth = 0;
    for (size_t i = open; i < pat.size(); ++i) {
        if (pat[i] == '{') ++depth;
        else if (pat[i] == '}' && --depth == 0) { close = i; break; }
    }
    if (close == std::string::npos) {  /* непарная '{': литерал */
        out.push_back(pat);
        return;
    }
    const std::string body = pat.substr(open + 1, close - open - 1);

    std::vector<std::string> alts;
    std::string cur;
    depth = 0;
    for (char c : body) {
        if (c == '{') ++depth;
        if (c == '}') --depth;
        if (c == ',' && depth == 0) {
            alts.push_back(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    alts.push_back(cur);

    const bool has_nested = body.find('{') != std::string::npos;
    if (alts.size() == 1 && !has_nested) {  /* {a} — литерал, как в bash */
        out.push_back(pat);
        return;
    }
    for (const auto& alt : alts)
        expand_braces(pat.substr(0, open) + alt + pat.substr(close + 1), out);
}

/* Сопоставить список сегментов шаблона со списком сегментов пути.
 * Сегмент '**' — рекурсивный спуск: ноль сегментов или любое их число.
 * Состояния мемоизируются, иначе два '**' подряд на глубоком пути дали бы
 * экспоненциальное время. */
bool match_segments(const std::vector<std::string>& pat,
                    const std::vector<std::string>& txt) {
    std::vector<std::vector<signed char>> memo(
        pat.size() + 1, std::vector<signed char>(txt.size() + 1, -1));
    auto go = [&](auto&& self, size_t pi, size_t ti) -> bool {
        signed char& m = memo[pi][ti];
        if (m != -1) return m == 1;
        bool r;
        if (pi == pat.size()) {
            r = (ti == txt.size());
        } else if (pat[pi] == "**") {
            r = self(self, pi + 1, ti);  /* ноль сегментов */
            for (size_t k = ti; !r && k < txt.size(); ++k)
                r = self(self, pi + 1, k + 1);
        } else {
            r = ti < txt.size() && match_segment(pat[pi], txt[ti]) &&
                self(self, pi + 1, ti + 1);
        }
        m = r ? 1 : 0;
        return r;
    };
    return go(go, 0, 0);
}

/* Привести шаблон к каноническому виду: '/' вместо '\', без ведущего
 * './' и без повторных разделителей. */
std::string normalize_pattern(const std::string& raw) {
    std::string s;
    s.reserve(raw.size());
    for (char c : raw) s += (c == '\\') ? '/' : c;
    /* Схлопываем '//' — иначе пустой сегмент ломал бы разбиение. */
    std::string t;
    t.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '/' && !t.empty() && t.back() == '/') continue;
        t += s[i];
    }
    while (!t.empty() && t.front() == '/') t.erase(t.begin());
    if (t.rfind("./", 0) == 0) t = t.substr(2);
    while (!t.empty() && t.back() == '/') t.pop_back();
    return t;
}

std::string to_lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

} // anonymous namespace

const std::vector<std::string>& default_skip_dirs() {
    static const std::vector<std::string> dirs = {".git", "node_modules",
                                                  "vendor", "__pycache__"};
    return dirs;
}

bool match_pattern(const std::string& pattern, const std::string& path) {
    std::vector<std::string> expanded;
    expand_braces(normalize_pattern(pattern), expanded);
    const std::vector<std::string> txt = split_segments(normalize_pattern(path));
    for (const auto& p : expanded) {
        if (match_segments(split_segments(p), txt)) return true;
    }
    return false;
}

Result find(const std::string& root, const std::string& pattern,
            const Options& options) {
    Result res;
    if (root.empty()) {
        res.error = "не задан каталог поиска";
        return res;
    }
    /* Шаблон из одних пробелов — вырожденный вызов модели. Отвечать
     * «ничего не найдено» нельзя: она ужесточит шаблон вместо того, чтобы
     * его задать. */
    if (pattern.find_first_not_of(" \t\r\n") == std::string::npos) {
        res.error = "пустой pattern — укажи шаблон, например **/*.php";
        return res;
    }

    std::error_code ec;
    const fs::path base(root);
    if (!fs::exists(base, ec)) {
        res.error = "каталог не существует: " + root;
        return res;
    }
    if (!fs::is_directory(base, ec)) {
        res.error = "не каталог: " + root;
        return res;
    }

    /* Модели присылают и абсолютный шаблон. Если он внутри корня —
     * отбрасываем префикс, иначе поиск идёт в никуда. Сделать это нужно
     * ДО normalize_pattern: тот срезает ведущий '/', после чего префикс
     * корня уже не распознать (первая версия так и делала). */
    std::string raw;
    raw.reserve(pattern.size());
    for (char c : pattern) raw += (c == '\\') ? '/' : c;
    std::string root_norm = base.lexically_normal().generic_string();
    while (root_norm.size() > 1 && root_norm.back() == '/') root_norm.pop_back();
    if (raw.rfind(root_norm + "/", 0) == 0) {
        raw = raw.substr(root_norm.size() + 1);
    } else if (raw == root_norm) {
        raw.clear();
    }
    const std::string pat = normalize_pattern(raw);
    if (pat.empty()) {
        res.error = "шаблон не задаёт ничего для поиска";
        return res;
    }

    std::vector<std::string> skip = options.skip_dirs;
    if (skip.empty()) skip = default_skip_dirs();

    std::vector<std::string> exts;
    for (const auto& e : options.extensions) {
        std::string v = to_lower(e);
        if (!v.empty() && v.front() != '.') v.insert(v.begin(), '.');
        if (v != ".") exts.push_back(v);
    }

    std::vector<Entry> found;
    for (auto it = fs::recursive_directory_iterator(
             base, fs::directory_options::none, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (++res.visited > options.max_visited) {
            res.walk_capped = true;
            break;
        }
        std::error_code ec2;
        const fs::path p = it->path();
        const bool is_dir = it->is_directory(ec2);
        /* Глубина: число разделителей относительно корня обхода. Считаем
         * по пути, а не по счётчику обхода, — иначе пропущенный каталог
         * сдвинул бы глубину и тихо обрезал выдачу. */
        if (options.max_depth > 0) {
            const std::string rel_probe =
                p.lexically_relative(base).generic_string();
            size_t depth = 0;
            for (char ch : rel_probe) {
                if (ch == '/') ++depth;
            }
            if (depth + 1 > options.max_depth) {
                if (is_dir) it.disable_recursion_pending();
                continue;
            }
        }
        if (is_dir) {
            const std::string name = p.filename().string();
            if (std::find(skip.begin(), skip.end(), name) != skip.end()) {
                /* Пропускаем поддерево целиком, а не только сам каталог. */
                it.disable_recursion_pending();
                continue;
            }
            if (!options.include_dirs) continue;
        } else if (!it->is_regular_file(ec2)) {
            continue;  /* сокеты, фифо — не файлы проекта */
        }

        const std::string rel =
            p.lexically_relative(base).generic_string();
        if (rel.empty() || rel == ".") continue;
        if (!match_pattern(pat, rel)) continue;
        if (!exts.empty() && !is_dir) {
            const std::string ext = to_lower(p.extension().string());
            if (std::find(exts.begin(), exts.end(), ext) == exts.end()) continue;
        }

        Entry e;
        e.path = rel;
        e.abs_path = p.string();
        e.name = p.filename().string();
        e.is_dir = is_dir;
        std::error_code ec3;
        const auto t = fs::last_write_time(p, ec3);
        if (!ec3)
            e.mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             t.time_since_epoch())
                             .count();
        found.push_back(std::move(e));
    }

    res.matched = found.size();

    if (options.newest_first) {
        std::stable_sort(found.begin(), found.end(),
                         [](const Entry& a, const Entry& b) {
                             if (a.mtime_ns != b.mtime_ns)
                                 return a.mtime_ns > b.mtime_ns;
                             return a.path < b.path;  /* детерминизм при равных */
                         });
    } else {
        std::sort(found.begin(), found.end(),
                  [](const Entry& a, const Entry& b) { return a.path < b.path; });
    }

    if (found.size() > options.limit) {
        found.resize(options.limit);
        res.truncated = true;
    }
    res.entries = std::move(found);
    return res;
}

} // namespace fileglob
} // namespace coder
