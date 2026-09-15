#include "base_tools.h"
#include "tools_registry.h"
#include "engine.h"
#include "project.h"
#include "security.h"
#include "shell.h"
#include "limits.h"
#include "file_utils.h"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <regex>
#include <algorithm>
#include <utility>
#include <iostream>

namespace fs = std::filesystem;
namespace coder {

namespace {

const std::vector<std::string> kSkipDirs = {".git", "node_modules", "vendor",
                                            "__pycache__"};

/* Лимиты вывода — единый источник: core/limits.h (Фаза 4.5). */
using limits::kMaxToolOutput;
using limits::kMaxGrepMatches;
using limits::kReadFileChars;
using limits::kMaxSymFile;

/* Чтение файла с ограничением размера и пропуском первых skip_lines строк. */
std::string read_text_file(const std::string& path, size_t max_chars, size_t skip_lines) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "[ошибка] не удалось открыть файл: " + path;

    std::string out;
    out.reserve(std::min<size_t>(max_chars, 65536));
    size_t line = 0;
    std::string ln;
    while (std::getline(f, ln)) {
        if (line++ < skip_lines) continue;
        if (out.size() + ln.size() + 1 > max_chars) {
            size_t room = max_chars - out.size();
            if (out.size() < max_chars) {
                out.append(ln, 0, room);
                out += "\n";
            }
            break;
        }
        out += ln;
        out += '\n';
    }
    return out;
}

/* Проверка доступа к пути за пределами корня проекта.
 * Если возвращает непустую строку — это сообщение об отказе (и движок
 * уже перевёл агента в режим ожидания разрешения пользователя). */
std::string guard_permission(const std::string& abs_path) {
    auto& eng = engine();
    return eng.check_external_permission(abs_path);
}

/* Резервная копия файла (.orig) перед правкой — для undo_edit (3.5).
 * Хранится только последний backup на файл (перезаписывается). */
void backup_file(const std::string& abs) {
    if (!fs::exists(abs)) return;
    std::error_code ec;
    fs::copy_file(abs, abs + ".orig", fs::copy_options::overwrite_existing, ec);
}

/* Разрешить относительный путь относительно корня проекта.
 * Единая реализация — project_resolve() (см. core/project.h). */

/* grep_search: поиск по файлам — НАСТОЯЩЕЕ регулярное выражение (ECMAScript). */
std::string base_grep(const std::string& root, const std::string& pattern) {
    if (pattern.empty())
        return "[ошибка] пустой PATTERN — укажи регулярное выражение";

    std::regex re;
    try {
        re = std::regex(pattern, std::regex::ECMAScript);
    } catch (const std::regex_error& e) {
        std::string err = e.what();
        return "[ошибка] невалидное регулярное выражение '" + pattern + "': "
               + err + ". Исправь PATTERN, не повторяй тот же самый.";
    }

    std::string base = root.empty() ? engine_state().project_dir : root;
    if (base.empty()) return "[ошибка] не задан ROOT и не задан project_dir";
    if (!fs::exists(base) || !fs::is_directory(base))
        return "[ошибка] каталог не существует: " + base;

    std::vector<std::string> files;
    file_utils::walk_files(base, files, 2000, kSkipDirs);

    std::stringstream out;
    size_t found = 0;
    for (const auto& fp : files) {
        std::ifstream f(fp);
        if (!f) continue;
        std::string line;
        size_t ln = 0;
        while (std::getline(f, line)) {
            ++ln;
            std::smatch m;
            if (std::regex_search(line, m, re)) {
                out << fp << ":" << ln << "  " << line << "\n";
                if (out.str().size() >= kMaxToolOutput) {
                    out << "\n[вывод обрезан по лимиту]";
                    return out.str();
                }
                if (++found >= kMaxGrepMatches) {
                    out << "\n[найдено совпадений: >= " << found << " — лимит вывода]";
                    return out.str();
                }
            }
        }
    }
    out << "[найдено совпадений: " << found << " в " << files.size() << " файлах]";
    return out.str();
}

/* repo_map: компактный обзор каталога (с кэшем на текущую задачу — B2). */
std::string base_repo_map(const std::string& root) {
    std::string base = root.empty() ? engine_state().project_dir : root;
    if (base.empty()) return "[ошибка] не задан ROOT и не задан project_dir";

    /* Кэш: если за эту задачу то же корень уже обозревали — возвращаем готовое. */
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        if (engine_state().repo_map_cache_root == base &&
            !engine_state().repo_map_cache_result.empty()) {
            return engine_state().repo_map_cache_result +
                   "\n[repo_map: из кэша задачи]";
        }
    }

    std::vector<std::string> files;
    file_utils::walk_files(base, files, 1500, kSkipDirs);
    if (files.empty()) return "[repo_map: файлы не найдены в " + base + "]";

    std::stringstream out;
    out << "[repo_map] " << files.size() << " файлов:\n";
    for (const auto& fp : files) {
        if (out.str().size() >= kMaxToolOutput) {
            out << "...\n[repo_map: вывод обрезан]";
            break;
        }
        std::string rel = fp;
        if (!base.empty() && rel.rfind(base, 0) == 0)
            rel = rel.substr(base.size() + 1);
        std::ifstream f(fp);
        if (!f) continue;
        std::string line;
        std::vector<std::string> syms;
        while (std::getline(f, line) && syms.size() < kMaxSymFile) {
            size_t kw = std::string::npos;
            if ((kw = line.find("function ")) != std::string::npos) {
                size_t start = kw + 9;
                size_t end = start;
                while (end < line.size() && (std::isalnum(line[end]) || line[end] == '_'))
                    ++end;
                if (end > start)
                    syms.push_back("f:" + line.substr(start, end - start));
            } else if ((kw = line.find("class ")) != std::string::npos) {
                size_t start = kw + 6;
                size_t end = start;
                while (end < line.size() && (std::isalnum(line[end]) || line[end] == '_'))
                    ++end;
                if (end > start)
                    syms.push_back("c:" + line.substr(start, end - start));
            }
            if (syms.size() < kMaxSymFile) {
                const char* hooks[] = {"add_action", "add_filter", "add_shortcode"};
                for (const char* hook : hooks) {
                    size_t hk = line.find(hook);
                    if (hk == std::string::npos) continue;
                    size_t lp = line.find('(', hk);
                    if (lp == std::string::npos) continue;
                    size_t q1 = line.find('\'', lp + 1);
                    if (q1 == std::string::npos) q1 = line.find('"', lp + 1);
                    if (q1 == std::string::npos) continue;
                    char q = line[q1];
                    size_t q2 = line.find(q, q1 + 1);
                    if (q2 == std::string::npos) continue;
                    syms.push_back("h:" + line.substr(q1 + 1, q2 - q1 - 1));
                    break;
                }
            }
        }
        out << "  " << rel;
        if (!syms.empty()) {
            out << "  [";
            for (size_t i = 0; i < syms.size(); ++i) {
                if (i) out << ", ";
                out << syms[i];
            }
            out << "]";
        }
        out << "\n";
    }
    std::string result = out.str();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().repo_map_cache_root = base;
        engine_state().repo_map_cache_result = result;
    }
    return result;
}

/* list_skills: имена и описания доступных навыков. */
std::string base_list_skills() {
    const auto& skills = SkillsManager::instance().all_skills();
    if (skills.empty()) return "[навыков нет]";
    std::stringstream s;
    s << "[навыков " << skills.size() << "]:\n";
    for (const auto& sk : skills)
        s << "  " << sk.name << " — " << sk.description << "\n";
    return s.str();
}

/* skill_detail: полный текст навыка по имени. */
std::string base_skill_detail(const std::string& name) {
    if (name.empty()) return "[ошибка] укажи имя навыка (QUERY)";
    const Skill* sk = SkillsManager::instance().find(name);
    if (!sk) {
        std::stringstream s;
        s << "[ошибка] навык '" << name << "' не найден. Доступные навыки:\n";
        const auto& skills = SkillsManager::instance().all_skills();
        for (const auto& s2 : skills)
            s << "  " << s2.name << " — " << s2.description << "\n";
        return s.str();
    }
    if (sk->body.empty())
        return "[" + sk->name + " — нет подробной инструкции]";
    return "### НАВЫК: " + sk->name + "\n" + sk->body;
}

} // anonymous namespace

void register_base_tools() {
    auto& reg = ToolsRegistry::instance();

    reg.register_tool("read_file", [](const ToolArgs& a) -> std::string {
        std::string abs = project_resolve(a.path);
        std::string perm = guard_permission(abs);
        if (!perm.empty()) return perm;
        size_t skip = (a.k > 1) ? static_cast<size_t>(a.k - 1) : 0;
        std::string content = read_text_file(abs, kReadFileChars, skip);
        std::stringstream hdr;
        hdr << "# read: " << abs;
        if (skip > 0) hdr << " (строки с " << skip + 1 << ")";
        if (content.size() >= kReadFileChars)
            hdr << " [обрезано по лимиту — читай с нужной строки через K]";
        return hdr.str() + "\n" + content;
    }, "Чтение файла");

    reg.register_tool("write_file", [](const ToolArgs& a) -> std::string {
        auto& st = engine_state();
        std::string abs = project_resolve(a.path);
        if (st.plan_mode) {
            std::lock_guard<std::mutex> lk(st.mtx);
            st.pending.push_back({a.path, a.content});
            return "[предложено, НЕ применено] " + abs
                   + " (" + std::to_string(a.content.size()) + " байт)";
        }
        if (!security::is_path_safe(a.path))
            return "[запрещено] небезопасный путь: " + a.path;
        if (!security::is_path_not_dangerous(abs))
            return "[запрещено] запись запрещена в: " + abs;
        std::string perm = guard_permission(abs);
        if (!perm.empty()) return perm;
        backup_file(abs);  // для undo_edit (3.5)
        std::ofstream f(abs, std::ios::binary);
        if (!f) return "[ошибка] не удалось записать: " + abs;
        f << a.content;
        f.close();
        return "[записано] " + abs + " (" + std::to_string(a.content.size()) + " байт)";
    }, "Запись файла");

    reg.register_tool("repo_map", [](const ToolArgs& a) -> std::string {
        return base_repo_map(a.root);
    }, "Обзор структуры каталога");

    reg.register_tool("grep_search", [](const ToolArgs& a) -> std::string {
        return base_grep(a.root, a.pattern);
    }, "Поиск по файлам (регулярное выражение)");

    reg.register_tool("list_skills", [](const ToolArgs&) -> std::string {
        return base_list_skills();
    }, "Список навыков");

    reg.register_tool("skill_detail", [](const ToolArgs& a) -> std::string {
        return base_skill_detail(a.query);
    }, "Полный текст навыка");

    /* search_replace: поиск и замена текста в файле (diff-based edit). */
    reg.register_tool("search_replace", [](const ToolArgs& a) -> std::string {
        std::string abs = project_resolve(a.path);
        std::ifstream fin(abs, std::ios::binary);
        if (!fin) return "[ошибка] не удалось открыть файл: " + abs;
        std::string content((std::istreambuf_iterator<char>(fin)),
                            std::istreambuf_iterator<char>());
        fin.close();

        if (a.query.empty()) return "[ошибка] пустой поисковый запрос (QUERY)";
        size_t pos = content.find(a.query);
        if (pos == std::string::npos)
            return "[search_replace] текст не найден в " + abs;

        size_t count = 0;
        size_t search_from = 0;
        while ((pos = content.find(a.query, search_from)) != std::string::npos) {
            count++;
            search_from = pos + 1;
        }
        if (count > 1)
            return "[search_replace] НАЙДЕНО " + std::to_string(count)
                   + " ВХОЖДЕНИЙ. Уточни запрос (добавь контекст вокруг замены).";

        pos = content.find(a.query);
        content.replace(pos, a.query.size(), a.content);

        if (engine_state().plan_mode) {
            std::lock_guard<std::mutex> lk(engine_state().mtx);
            engine_state().pending.push_back({a.path, content});
            return "[предложено] " + abs + " (search_replace, "
                   + std::to_string(a.query.size()) + " -> "
                   + std::to_string(a.content.size()) + " байт)";
        }

        std::string perm = guard_permission(abs);
        if (!perm.empty()) return perm;

        backup_file(abs);  // для undo_edit (3.5)
        std::ofstream fout(abs, std::ios::binary | std::ios::trunc);
        if (!fout) return "[ошибка] не удалось записать: " + abs;
        fout << content;
        fout.close();
        return "[search_replace] " + abs + ": заменено " + std::to_string(a.query.size())
               + " -> " + std::to_string(a.content.size()) + " байт";
    }, "Поиск и замена текста в файле");

    /* exec_command: запуск shell-команды с таймаутом. */
    reg.register_tool("exec_command", [](const ToolArgs& a) -> std::string {
        if (a.cli.empty()) return "[ошибка] пустая команда (CLI)";
        if (!security::is_command_allowed(a.cli))
            return "[запрещено] команда заблокирована политикой безопасности";
        std::string out = shell::run_capture(a.cli, 60);
        return out.empty() ? "[exec: нет вывода]" : shell::cap(out, kMaxToolOutput);
    }, "Запуск shell-команды");

    /* 3.1 list_dir: лёгкий список файлов/каталогов (в отличие от repo_map). */
    reg.register_tool("list_dir", [](const ToolArgs& a) -> std::string {
        std::string dir = a.path.empty() ? engine_state().project_dir : a.path;
        if (dir.empty()) return "[ошибка] пустой PATH — укажи каталог";
        if (!fs::exists(dir) || !fs::is_directory(dir))
            return "[ошибка] каталог не существует: " + dir;
        std::string perm = guard_permission(dir);
        if (!perm.empty()) return perm;
        size_t limit = (a.k > 0) ? static_cast<size_t>(a.k) : 100;
        std::stringstream out;
        out << "[list_dir] " << dir << ":\n";
        std::error_code ec;
        size_t count = 0;
        for (auto it = fs::directory_iterator(dir, ec);
             it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const auto& p = it->path();
            std::string name = p.filename().string();
            if (it->is_directory()) name += "/";
            out << name << "\n";
            if (++count >= limit) {
                out << "...[лимит " << limit << " записей]";
                break;
            }
        }
        return out.str();
    }, "Список файлов в каталоге");

    /* 3.2 web_fetch: HTTP GET через curl с лимитом вывода. */
    reg.register_tool("web_fetch", [](const ToolArgs& a) -> std::string {
        if (a.url.empty()) return "[ошибка] пустой URL (URL)";
        std::string cmd = "curl -s -L -m 30 --max-redirs 3 " + shell::shell_quote(a.url);
        std::string out = shell::run_capture(cmd, 40);
        if (out.empty()) return "[web_fetch: пустой ответ]";
        return shell::cap(out, kMaxToolOutput);
    }, "HTTP GET запрос (curl)");

    /* 3.3 edit_file: замена диапазона строк.
     * K: строка начала (1-based), QUERY: строка конца (1-based, включительно,
     * пусто = только K), CONTENT: новый текст строк. */
    reg.register_tool("edit_file", [](const ToolArgs& a) -> std::string {
        std::string abs = project_resolve(a.path);
        if (a.k <= 0) return "[ошибка] укажи K: номер строки начала (1-based)";
        std::ifstream fin(abs, std::ios::binary);
        if (!fin) return "[ошибка] не удалось открыть файл: " + abs;
        std::vector<std::string> lines;
        std::string ln;
        while (std::getline(fin, ln)) lines.push_back(ln);
        fin.close();
        if (lines.empty()) return "[ошибка] пустой файл: " + abs;

        size_t start = static_cast<size_t>(a.k);
        size_t end = start;
        if (!a.query.empty()) {
            try { end = static_cast<size_t>(std::stoul(a.query)); }
            catch (...) { return "[ошибка] невалидный QUERY (строка конца): " + a.query; }
        }
        if (start < 1 || start > lines.size() || end < start || end > lines.size())
            return "[ошибка] диапазон строк вне файла: K=" + std::to_string(a.k)
                   + " QUERY=" + a.query + " (всего строк: " + std::to_string(lines.size()) + ")";

        /* Замена: start..end (1-based, включительно) на CONTENT. */
        std::vector<std::string> replacement;
        std::string rline;
        std::istringstream iss(a.content);
        while (std::getline(iss, rline)) replacement.push_back(rline);

        std::vector<std::string> result;
        result.reserve(lines.size() + replacement.size());
        for (size_t i = 0; i < start - 1; ++i) result.push_back(std::move(lines[i]));
        for (auto& r : replacement) result.push_back(std::move(r));
        for (size_t i = end; i < lines.size(); ++i) result.push_back(std::move(lines[i]));

        std::string new_content;
        for (const auto& l : result) { new_content += l; new_content += '\n'; }

        if (engine_state().plan_mode) {
            std::lock_guard<std::mutex> lk(engine_state().mtx);
            engine_state().pending.push_back({a.path, new_content});
            return "[предложено] " + abs + " (edit_file, строки " + std::to_string(start)
                   + "-" + std::to_string(end) + ")";
        }

        std::string perm = guard_permission(abs);
        if (!perm.empty()) return perm;
        backup_file(abs);  // для undo_edit (3.5)
        std::ofstream fout(abs, std::ios::binary | std::ios::trunc);
        if (!fout) return "[ошибка] не удалось записать: " + abs;
        fout << new_content;
        fout.close();
        return "[edit_file] " + abs + ": заменены строки " + std::to_string(start)
               + "-" + std::to_string(end);
    }, "Замена диапазона строк файла");

    /* 3.5 undo_edit: отмена последней правки из .orig-backup. */
    reg.register_tool("undo_edit", [](const ToolArgs& a) -> std::string {
        std::string abs = project_resolve(a.path);
        std::string bak = abs + ".orig";
        if (!fs::exists(bak)) return "[ошибка] нет backup (.orig) для " + abs;
        if (engine_state().plan_mode)
            return "[предложено] undo для " + abs;
        std::string perm = guard_permission(abs);
        if (!perm.empty()) return perm;
        std::error_code ec;
        fs::copy_file(bak, abs, fs::copy_options::overwrite_existing, ec);
        if (ec) return "[ошибка] восстановление не удалось: " + ec.message();
        return "[undo_edit] восстановлен файл: " + abs;
    }, "Отмена последней правки файла");
}

void register_rag_tools() {
    auto& reg = ToolsRegistry::instance();

    reg.register_tool("rag_index", [](const ToolArgs& a) -> std::string {
        std::string base = a.root.empty() ? engine_state().project_dir : a.root;
        if (base.empty()) return "[ошибка] не задан корень индексации";
        std::vector<std::string> files;
        file_utils::walk_files(base, files, 4000, kSkipDirs, ".php");
        const auto& cb = Engine::instance().callbacks();
        int ok = 0;
        for (const auto& fp : files) {
            if (cb.rag_process_document && cb.rag_process_document(fp)) ++ok;
        }
        std::stringstream s;
        s << "[проиндексировано " << ok << "/" << files.size() << " php-файлов в RAG]";
        return s.str();
    }, "Индексация в RAG");

    reg.register_tool("rag_query", [](const ToolArgs& a) -> std::string {
        const auto& cb = Engine::instance().callbacks();
        if (!cb.rag_build_prompt) return "[ошибка] RAG не доступен";
        std::string result = cb.rag_build_prompt(a.query, a.k, "");
        if (result.empty()) return "[RAG: пусто — проект не проиндексирован]";
        return shell::cap(result, kMaxToolOutput);
    }, "Поиск в RAG");
}

} // namespace coder