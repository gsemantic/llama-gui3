/*
 * instruction.cpp — загрузка инструкций проекта и пользователя (И9.1).
 *
 * Заголовок объясняет замысел; здесь — только детали, каждая из которых
 * закрывает способ тихо не сделать то, ради чего задача написана:
 *
 *   - источник, который не прочитался, НЕ попадает в промпт (иначе текст
 *     ошибки от curl стал бы «инструкцией»);
 *   - один и тот же файл попадает в список один раз (иначе настройка,
 *     повторяющая AGENTS.md, тихо удваивает правила);
 *   - результат шаблона сортируется по пути (иначе порядок обхода
 *     каталогов — unspecified, и один и тот же проект давал бы разный
 *     промпт от запуска к запуску, а кэш промпта это не различает).
 */

#include "instruction.h"

#include "glob.h"
#include "json.h"
#include "limits.h"
#include "security.h"
#include "shell.h"
#include "text_edit.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

namespace fs = std::filesystem;

namespace coder {
namespace instruction {

namespace {

/* Текст источника → текст для промпта.
 *
 * Через text_edit.h, а не свой разбор байтов: формат файла (BOM, CRLF)
 * читает ровно один модуль, и вторая реализация здесь означала бы, что
 * правка текстового формата когда-нибудь дойдёт до одной из них.
 *
 * Снимаются ОБА признака формата, и это не косметика:
 *   - BOM в промпте невидим и приклеивается к слову первой строки,
 *     то есть правило начинается с мусора;
 *   - CRLF сохраняется join_text'ом обратно — файл, написанный на
 *     Windows, принёс бы в промпт «\r» в конце каждой строки, и модель
 *     считала бы их частью текста правил (а ещё и режется по ним вывод
 *     инструментов, то есть это уже не только вопрос промпта).
 *
 * Одна функция на файл и на URL: правило «как выглядит инструкция в
 * промпте» не должно расходиться между сетевым и файловым источником. */
std::string normalize_for_prompt(const std::string& raw) {
    TextFile tf = split_text(raw);
    tf.bom.clear();
    tf.crlf = false;
    return join_text(tf);
}

/* Прочитать файл инструкции.
 *
 * Возвращает false, если прочитать не удалось ИЛИ это не обычный файл:
 * каталог и устройство не являются инструкцией, а молчаливый пустой
 * блок выглядел бы как «правил нет». */
bool read_instruction_file(const fs::path& path, std::string& out) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    const std::string raw((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    if (f.bad()) return false;
    std::string body = normalize_for_prompt(raw);
    if (body.size() > limits::kMaxInstructionChars) {
        body.resize(limits::kMaxInstructionChars);
        body += "\n[обрезано: источник длиннее " +
                std::to_string(limits::kMaxInstructionChars) +
                " символов, хвост в промпт не вошёл]";
    }
    out = std::move(body);
    return true;
}

/* Подпись источника для модели: относительно корня проекта, если файл
 * внутри него, иначе полным путём. Две строки с одним именем из
 * разных каталогов должны различаться в глазах модели, а не сливаться. */
std::string label_for(const fs::path& abs, const std::string& project_dir) {
    if (!project_dir.empty()) {
        const std::string root =
            fs::weakly_canonical(fs::path(project_dir)).generic_string();
        const std::string full = abs.generic_string();
        if (full.size() > root.size() && full.rfind(root + "/", 0) == 0) {
            return full.substr(root.size() + 1);
        }
    }
    return abs.generic_string();
}

/* Накопитель источников: дедупликация, обрезка и разбор файла в одном
 * месте, чтобы каждый путь добавления не реализовывал их заново. */
class Collector {
public:
    explicit Collector(const std::string& project_dir)
        : project_dir_(project_dir) {}

    void add_file(const fs::path& abs, const std::string& label_hint = "") {
        std::error_code ec;
        const fs::path full = fs::weakly_canonical(abs, ec);
        if (ec) return;
        /* Ключ — канонический путь, а не как записали: «AGENTS.md»,
         * «./AGENTS.md» и «/home/user/p/../p/AGENTS.md» — один файл, и
         * три блока с его содержимым означали бы три разных правила. */
        const std::string key = full.generic_string();
        if (!seen_.insert(key).second) {
            note("уже загружен, повтор пропущен: " + key);
            return;
        }
        std::string body;
        if (!read_instruction_file(full, body)) {
            seen_.erase(key);
            note("не прочитан: " + full.generic_string());
            return;
        }
        Instruction in;
        in.origin = full.generic_string();
        in.label = label_hint.empty()
            ? label_for(full, project_dir_)
            : label_hint;
        in.body = std::move(body);
        items_.push_back(std::move(in));
    }

    /* URL: отдельный путь, потому что файла за ним нет и дедупликация
     * идёт по самому адресу. */
    void add_url(const std::string& url, const std::string& body) {
        if (body.empty()) {
            note("пустой ответ: " + url);
            return;
        }
        if (!seen_.insert("url:" + url).second) {
            note("уже загружен, повтор пропущен: " + url);
            return;
        }
        Instruction in;
        in.origin = url;
        in.label = url;
        in.body = body.size() > limits::kMaxInstructionChars
            ? body.substr(0, limits::kMaxInstructionChars) +
                  "\n[обрезано: источник длиннее " +
                  std::to_string(limits::kMaxInstructionChars) +
                  " символов, хвост в промпт не вошёл]"
            : body;
        items_.push_back(std::move(in));
    }

    std::vector<Instruction> take() { return std::move(items_); }

private:
    static void note(const std::string& text) {
        std::cerr << "[wp_coder] инструкции: " << text << std::endl;
    }
    std::vector<Instruction> items_;
    std::set<std::string> seen_;
    std::string project_dir_;
};

/* Разделить АБСОЛУТНЫЙ шаблон на корень обхода и сам шаблон.
 *
 * Корень — часть до первого символа шаблона, обрезанная до последнего
 * разделителя. Без этого «rules/звезда.md» в каталоге rules обошёл бы весь корень
 * файловой системы и упёрся в предохранитель обхода (kGlobMaxVisited),
 * то есть ответ зависел бы от того, что лежит в /dev и /proc. */
void split_absolute_pattern(const std::string& pattern, std::string& root,
                            std::string& rest) {
    const size_t magic = pattern.find_first_of("*?[{");
    const size_t cut = (magic == std::string::npos) ? pattern.size() : magic;
    const size_t slash = pattern.rfind('/', cut);
    if (slash == std::string::npos || slash == 0) {
        root = "/";
        rest = pattern.substr(1);
        return;
    }
    root = pattern.substr(0, slash);
    rest = pattern.substr(slash + 1);
}

} // namespace

bool fetch_url(const std::string& url, std::string& out) {
    out.clear();
    const std::string cmd =
        "curl -fsSL --max-time " +
        std::to_string(limits::kInstructionFetchTimeoutSec) + " " +
        security::shell_escape(url);
    std::string text;
    int code = 0;
    /* run_capture_status, а не run_capture: по коду возврата видно,
     * что URL не отдал содержимое. Иначе страница с HTTP-ошибкой (или
     * пустой ответ) стала бы инструкцией, и модель применяла бы её. */
    if (!shell::run_capture_status(cmd, text, code,
                                   limits::kInstructionFetchTimeoutSec)) {
        return false;
    }
    if (text.empty()) return false;
    out = normalize_for_prompt(text);
    return !out.empty();
}

std::string expand_home(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    if (path.size() > 1 && path[1] != '/') return path;  /* ~user — не наш */
    const char* home = std::getenv("HOME");
    if (!home || !home[0]) return "";
    std::string h = home;
    while (h.size() > 1 && h.back() == '/') h.pop_back();
    if (path == "~") return h;
    return h + path.substr(1);
}

std::vector<std::string> parse_config_list(const std::string& json_text) {
    std::vector<std::string> out;
    if (json_text.find_first_not_of(" \t\r\n") == std::string::npos) return out;
    json::JsonValue root;
    if (!json::JsonValue::parse(json_text, root) || !root.is_array()) {
        /* Не массив строк — не молчим: пользователь настроек должен узнать,
         * что его список не прочитан, иначе он будет ждать инструкции,
         * которых не будет. */
        std::cerr << "[wp_coder] инструкции: wp_coder.instructions — ожидается "
                     "JSON-массив строк, например [\"docs/rules.md\", "
                     "\"https://example.com/rules.md\"]" << std::endl;
        return out;
    }
    for (size_t i = 0; i < root.size(); ++i) {
        const json::JsonValue& item = root.at(i);
        if (!item.is_string()) continue;
        std::string v = item.as_string();
        /* Обрезка пробелов: неразобранный пробел сделал бы «» имён
         * файла, и источник пропал бы без единого слова. */
        const size_t b = v.find_first_not_of(" \t\r\n");
        const size_t e = v.find_last_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        out.push_back(v.substr(b, e - b + 1));
    }
    return out;
}

bool has_glob_magic(const std::string& pattern) {
    return pattern.find_first_of("*?[{") != std::string::npos;
}

std::vector<Instruction> load(const std::string& project_dir,
                              const std::string& config_instructions,
                              const UrlFetcher& fetch) {
    Collector c(project_dir);

    /* 1. Общие для пользователя. Только AGENTS.md — так в плане, и
     * заводить второе имя здесь означало бы правило, о котором никто не
     * просил. */
    const std::string home = expand_home("~");
    if (home.empty()) {
        std::cerr << "[wp_coder] инструкции: HOME не задан, "
                     "~/.config/wp_coder/AGENTS.md не читается" << std::endl;
    } else {
        c.add_file(fs::path(home) / ".config" / "wp_coder" / "AGENTS.md",
                   "~/.config/wp_coder/AGENTS.md");
    }

    /* 2. Корень проекта. AGENTS.md раньше CLAUDE.md: у имён есть
     * приоритет, и «приоритет имён» из 9.8 проверяется именно на нём. */
    if (!project_dir.empty()) {
        for (const char* name : {"AGENTS.md", "CLAUDE.md"}) {
            c.add_file(fs::path(project_dir) / name);
        }
    }

    /* 3. Явно перечисленные источники, в порядке настройки. */
    for (const std::string& entry : parse_config_list(config_instructions)) {
        if (entry.rfind("http://", 0) == 0 || entry.rfind("https://", 0) == 0) {
            std::string body;
            if (fetch && fetch(entry, body)) {
                c.add_url(entry, body);
            } else {
                std::cerr << "[wp_coder] инструкции: не загружен " << entry
                          << std::endl;
            }
            continue;
        }
        const std::string raw = expand_home(entry);
        if (raw.empty() || raw[0] == '~') {
            std::cerr << "[wp_coder] инструкции: не раскрыт «~» в «" << entry
                      << "»" << std::endl;
            continue;
        }
        if (!has_glob_magic(raw)) {
            const fs::path abs = fs::path(raw).is_absolute()
                ? fs::path(raw)
                : fs::path(project_dir.empty() ? std::string(".") : project_dir) /
                      raw;
            c.add_file(abs);
            continue;
        }
        /* Шаблон. Относительный ищется от корня проекта: настройка
         * принадлежит проекту, и иначе шаблон «docs/звезда.md» означал бы разное в
         * двух открытых проектах. */
        std::string root;
        std::string pattern;
        if (fs::path(raw).is_absolute()) {
            split_absolute_pattern(raw, root, pattern);
        } else {
            if (project_dir.empty()) {
                std::cerr << "[wp_coder] инструкции: относительный шаблон «"
                          << entry << "» без корня проекта не ищется"
                          << std::endl;
                continue;
            }
            root = project_dir;
            pattern = raw;
        }
        fileglob::Options opt;
        /* Порядок — по пути, а не по времени изменения: инструкции
         * попадают в кэш промпта, и «свежие сверху» сделало бы его
         * содержимое зависящим от времени последнего касания. */
        opt.newest_first = false;
        const fileglob::Result res = fileglob::find(root, pattern, opt);
        if (!res.error.empty()) {
            std::cerr << "[wp_coder] инструкции: шаблон «" << entry
                      << "» — " << res.error << std::endl;
        }
        std::vector<fileglob::Entry> entries = res.entries;
        std::sort(entries.begin(), entries.end(),
                  [](const fileglob::Entry& a, const fileglob::Entry& b) {
                      return a.path < b.path;
                  });
        for (const fileglob::Entry& e : entries) {
            if (e.is_dir) continue;
            c.add_file(e.abs_path);
        }
        /* Обход мог быть остановлен предохранителем, и тогда найдено не
         * всё: молчать об этом нельзя, список инструкций выглядел бы
         * полным. */
        if (res.truncated || res.walk_capped) {
            std::cerr << "[wp_coder] инструкции: шаблон «" << entry
                      << "» — выдача неполная (сработал предел)"
                      << std::endl;
        }
    }

    return c.take();
}

std::string render(const std::vector<Instruction>& items) {
    std::string out;
    for (const Instruction& in : items) {
        if (in.body.empty()) continue;
        out += "\n\n## Instructions from: ";
        out += in.label;
        out += "\n\n";
        out += in.body;
    }
    return out;
}

std::vector<Instruction> resolve(const std::string& file,
                                 const std::string& project_dir) {
    Collector c(project_dir);
    if (file.empty() || project_dir.empty()) return c.take();

    std::error_code ec;
    const fs::path dir = fs::weakly_canonical(fs::path(file).parent_path(), ec);
    const fs::path root =
        fs::weakly_canonical(fs::path(project_dir), ec);
    if (ec) return c.take();

    /* Подъём заканчивается корнем проекта, и сравнение — каноническим
     * путём: «/home/u/p/../p» и «/home/u/p» — один каталог, и сравнение
     * строк остановило бы подъём там, где он на самом деле уже дошёл. */
    const bool inside =
        dir == root ||
        dir.generic_string().rfind(root.generic_string() + "/", 0) == 0;
    if (!inside) return c.take();

    fs::path cur = dir;
    for (int hops = 0; hops < limits::kInstructionWalkHops; ++hops) {
        for (const char* name : {"AGENTS.md", "CLAUDE.md"}) {
            c.add_file(cur / name);
        }
        if (cur == root) break;
        const fs::path up = cur.parent_path();
        if (up == cur) break;
        cur = up;
    }
    return c.take();
}

size_t append_new(std::vector<Instruction>& into,
                  std::vector<Instruction> more) {
    std::set<std::string> have;
    for (const Instruction& in : into) have.insert(in.origin);
    size_t added = 0;
    for (Instruction& in : more) {
        if (in.body.empty()) continue;
        if (!have.insert(in.origin).second) continue;
        into.push_back(std::move(in));
        ++added;
    }
    return added;
}

} // namespace instruction
} // namespace coder