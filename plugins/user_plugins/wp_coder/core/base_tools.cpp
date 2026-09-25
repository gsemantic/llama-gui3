#include "base_tools.h"
#include "tool.h"
#include "tools_registry.h"
#include "engine.h"
#include "project.h"
#include "security.h"
#include "shell.h"
#include "limits.h"
#include "file_utils.h"
#include "json_utils.h"

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
        ln = text::sanitize_utf8(ln);
        if (out.size() >= max_chars) break;
        if (out.size() + ln.size() + 1 > max_chars) {
            size_t room = max_chars - out.size();
            std::string part = text::utf8_prefix(ln, room);
            out += part;
            if (part.size() < room) out += '\n';
            break;
        }
        out += ln;
        out += '\n';
    }
    return out;
}

/* Доступ к пути за пределами корня проекта (И1.7): решение принимает
 * ToolContext, а инструмент только спрашивает. Если возвращается
 * непустая строка — это текст отказа, и агент уже переведён в режим
 * ожидания разрешения пользователя. */
std::string guard_permission(ToolContext& ctx, const std::string& abs_path) {
    return ctx.check_external_permission(abs_path);
}

/* Первый абсолютный путь в тексте команды, лежащий за пределами корня
 * проекта. Нужен, чтобы shell-инструменты спрашивали разрешение так же,
 * как файловые. Возвращает "" , если таких путей нет или корень проекта
 * не задан (тогда проверять нечего).
 *
 * Эвристика по токенам: выкидываем кавычки, берём токены, начинающиеся с
 * '/' и не являющиеся флагом, и сравниваем префикс с корнем проекта. */
std::string first_external_path(const std::string& cmd) {
    std::string proj = engine_state().project_dir;
    if (proj.empty()) return "";

    std::string cleaned;
    cleaned.reserve(cmd.size());
    for (char c : cmd) {
        if (c == '"' || c == '\'') continue;
        cleaned += c;
    }

    size_t i = 0;
    while (i < cleaned.size()) {
        while (i < cleaned.size() &&
               (cleaned[i] == ' ' || cleaned[i] == '\t' || cleaned[i] == '\n')) ++i;
        size_t start = i;
        while (i < cleaned.size() &&
               cleaned[i] != ' ' && cleaned[i] != '\t' && cleaned[i] != '\n') ++i;
        if (start == i) break;

        std::string tok = cleaned.substr(start, i - start);
        /* Отбрасываем аргументы вида --path=/x, --file=/x: префикс убираем. */
        size_t eq = tok.find('=');
        if (eq != std::string::npos && tok.size() > 2 && tok[0] == '-') {
            if (eq + 1 < tok.size() && tok[eq + 1] == '/') tok = tok.substr(eq + 1);
            else continue;
        }
        if (tok.size() < 2 || tok[0] != '/') continue;
        if (!is_path_outside(tok, proj)) continue;

        /* Не разбудить пользователя на системных путях, которые почти
         * всегда есть в аргументах и которые и так защищены политикой
         * команд (--path, --allow-root и т.п.). */
        if (is_path_allowed(tok, proj, engine_state().allowed_external_paths))
            continue;
        return tok;
    }
    return "";
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
std::string base_grep(const std::string& root, const std::string& pattern,
                    const std::string& project_dir) {
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

    std::string base = root.empty() ? project_dir : root;
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
std::string base_repo_map(const std::string& root,
                         const std::string& project_dir) {
    std::string base = root.empty() ? project_dir : root;
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

/* --- Маленькие помощники для чтения аргументов (И1.3) --- */

std::string arg_str(const json::JsonValue& a, const char* key) {
    return a.get_string(key);
}

int arg_int(const json::JsonValue& a, const char* key) {
    return static_cast<int>(a.get_int(key, 0));
}

ToolOutput out(std::string text) {
    ToolOutput o;
    o.output = std::move(text);
    return o;
}

ToolOutput out(std::string title, std::string text) {
    ToolOutput o;
    o.title = std::move(title);
    o.output = std::move(text);
    return o;
}

} // anonymous namespace

void register_base_tools() {
    auto& reg = ToolsRegistry::instance();

    {
        ToolDef def;
        def.name = "read_file";
        def.description = "Чтение текстового файла";
        def.flags = TF_READ_ONLY;
        def.permission_key = "read";
        SchemaBuilder b;
        b.str("path", "путь к файлу относительно корня проекта")
         .integer_range("k", "с какой строки читать (1 — с первой); 0 = с начала",
                        0, 1000000)
         .required("path");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            std::string abs = project_resolve(arg_str(a, "path"));
            std::string perm = guard_permission(ctx, abs);
            if (!perm.empty()) return out(std::move(perm));
            int k = arg_int(a, "k");
            size_t skip = (k > 1) ? static_cast<size_t>(k - 1) : 0;
            std::string content = read_text_file(abs, kReadFileChars, skip);
            std::stringstream hdr;
            hdr << "# read: " << abs;
            if (skip > 0) hdr << " (строки с " << skip + 1 << ")";
            if (content.size() >= kReadFileChars)
                hdr << " [обрезано по лимиту — читай с нужной строки через k]";
            return out("read " + arg_str(a, "path"), hdr.str() + "\n" + content);
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "write_file";
        def.description = "Запись текста в файл (создаёт или перезаписывает)";
        def.flags = TF_WRITES_FILES;
        def.permission_key = "write";
        SchemaBuilder b;
        b.str("path", "путь к файлу относительно корня проекта")
         .str("content", "полный новый содержимое файла")
         .required("path").required("content");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            const std::string rel = arg_str(a, "path");
            const std::string content = arg_str(a, "content");
            std::string abs = project_resolve(rel);
            /* И1.7: режим плана решается здесь, в контексте, а не в
             * инструменте. Раньше эта проверка была продублирована в
             * четырёх инструментах и отсутствовала в остальных. */
            if (ctx.propose_write(rel, content)) {
                return out("предложено " + rel, "[предложено, НЕ применено] " + abs
                           + " (" + std::to_string(content.size()) + " байт)");
            }
            if (!security::is_path_safe(rel))
                return out("[запрещено] небезопасный путь: " + rel);
            if (!security::is_path_not_dangerous(abs))
                return out("[запрещено] запись запрещена в: " + abs);
            std::string perm = guard_permission(ctx, abs);
            if (!perm.empty()) return out(std::move(perm));
            backup_file(abs);  // для undo_edit (3.5)
            std::ofstream f(abs, std::ios::binary | std::ios::trunc);
            if (!f) return out("[ошибка] не удалось записать: " + abs);
            f << content;
            f.close();
            return out("записано " + rel, "[записано] " + abs + " ("
                       + std::to_string(content.size()) + " байт)");
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "repo_map";
        def.description = "Обзор структуры каталога: файлы и найденные символы";
        def.flags = TF_READ_ONLY;
        def.permission_key = "read";
        SchemaBuilder b;
        b.str("root", "каталог; пусто = корень проекта");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            std::string root = arg_str(a, "root");
            return out("repo_map", base_repo_map(root, ctx.project_dir()));
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "grep_search";
        def.description = "Поиск по файлам регулярным выражением (ECMAScript)";
        def.flags = TF_READ_ONLY;
        def.permission_key = "read";
        SchemaBuilder b;
        b.str("root", "каталог; пусто = корень проекта")
         .str("pattern", "регулярное выражение")
         .required("pattern");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            return out("grep", base_grep(arg_str(a, "root"), arg_str(a, "pattern"),
                                        ctx.project_dir()));
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "list_skills";
        def.description = "Список доступных навыков (имени и описаний)";
        def.flags = TF_READ_ONLY;
        def.permission_key = "read";
        def.parameters = SchemaBuilder().build();
        def.handler = [](const json::JsonValue&, ToolContext&) -> ToolOutput {
            return out("навыки", base_list_skills());
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "skill_detail";
        def.description = "Полный текст навыка по имени";
        def.flags = TF_READ_ONLY;
        def.permission_key = "read";
        SchemaBuilder b;
        b.str("query", "имя навыка из list_skills").required("query");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            return out("навык", base_skill_detail(arg_str(a, "query")));
        };
        reg.register_def(std::move(def));
    }

    /* search_replace: поиск и замена текста в файле (diff-based edit). */
    {
        ToolDef def;
        def.name = "search_replace";
        def.description =
            "Поиск и замена текста в файле. Искомый текст должен встречаться"
            " РОВНО ОДИН раз — иначе добавь к нему контекст.";
        def.flags = TF_WRITES_FILES;
        def.permission_key = "write";
        SchemaBuilder b;
        b.str("path", "путь к файлу")
         .str("query", "что искать (должно быть уникальным в файле)")
         .str("content", "на что заменить")
         .required("path").required("query").required("content");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            const std::string rel = arg_str(a, "path");
            const std::string needle = arg_str(a, "query");
            const std::string replacement = arg_str(a, "content");
            std::string abs = project_resolve(rel);
            std::ifstream fin(abs, std::ios::binary);
            if (!fin) return out("[ошибка] не удалось открыть файл: " + abs);
            std::string content((std::istreambuf_iterator<char>(fin)),
                                std::istreambuf_iterator<char>());
            fin.close();

            if (needle.empty()) return out("[ошибка] пустой поисковый запрос (query)");
            size_t pos = content.find(needle);
            if (pos == std::string::npos)
                return out("[search_replace] текст не найден в " + abs);

            size_t count = 0;
            size_t search_from = 0;
            while ((pos = content.find(needle, search_from)) != std::string::npos) {
                count++;
                search_from = pos + 1;
            }
            if (count > 1)
                return out("[search_replace] НАЙДЕНО " + std::to_string(count)
                           + " ВХОЖДЕНИЙ. Уточни запрос (добавь контекст вокруг замены).");

            pos = content.find(needle);
            content.replace(pos, needle.size(), replacement);

            if (ctx.propose_write(rel, content)) {
                return out("предложено " + rel, "[предложено] " + abs + " (search_replace, "
                           + std::to_string(needle.size()) + " -> "
                           + std::to_string(replacement.size()) + " байт)");
            }

            std::string perm = guard_permission(ctx, abs);
            if (!perm.empty()) return out(std::move(perm));

            backup_file(abs);  // для undo_edit (3.5)
            std::ofstream fout(abs, std::ios::binary | std::ios::trunc);
            if (!fout) return out("[ошибка] не удалось записать: " + abs);
            fout << content;
            fout.close();
            return out("замена в " + rel, "[search_replace] " + abs + ": заменено "
                       + std::to_string(needle.size()) + " -> "
                       + std::to_string(replacement.size()) + " байт");
        };
        reg.register_def(std::move(def));
    }

    /* exec_command: запуск shell-команды с таймаутом. */
    {
        ToolDef def;
        def.name = "exec_command";
        def.description =
            "Запуск shell-команды. КРАЙНЯЯ мера: сначала пробуй"
            " специализированные инструменты. Таймаут 60 с.";
        /* DESTRUCTIVE: команда может сделать что угодно, и откатить это
         * инструмент не умеет. Именно поэтому он обязан быть виден
         * политике режимов и (в И2) подтверждению пользователя. */
        def.flags = TF_EXECUTES | TF_DESTRUCTIVE | TF_SLOW;
        def.permission_key = "bash";
        SchemaBuilder b;
        b.str("cli", "команда для выполнения в shell")
         .required("cli");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            std::string cmd = arg_str(a, "cli");
            if (cmd.empty()) return out("[ошибка] пустая команда (cli)");
            /* И3.6: allowlist вместо blocklist. Разбор команды снимает
             * кавычки и обходит конвейеры, поэтому «cat x | sh» —
             * это две команды, а не строка; программа сверх списка
             * не выполняется, а sudo -E / git -c core.pager= / rm -rf ~
             * отклоняются валидаторами. Отказ политики подтверждению
             * пользователем не подлежит: разрешение («bash → allow»)
             * не отменяет запрет, иначе политика была бы украшением. */
            std::string refusal = security::check_command(cmd);
            if (!refusal.empty())
                return out("[запрещено политикой команд] " + refusal);
            /* Команда упоминает путь за пределами проекта — спрашиваем
             * разрешение пользователя, как это делают файловые инструменты.
             * Раньше проверки не было вовсе: единственный инструмент, который
             * запускает произвольный код, был единственным без гейта. */
            std::string outside = first_external_path(cmd);
            if (!outside.empty()) {
                std::string refusal = guard_permission(ctx, outside);
                if (!refusal.empty()) return out(std::move(refusal));
            }
            std::string result = shell::run_capture(cmd, 60);
            return out("shell", result.empty() ? "[exec: нет вывода]"
                                               : shell::cap(result, kMaxToolOutput));
        };
        reg.register_def(std::move(def));
    }

    /* 3.1 list_dir: лёгкий список файлов/каталогов (в отличие от repo_map). */
    {
        ToolDef def;
        def.name = "list_dir";
        def.description = "Список файлов и каталогов (одного уровня)";
        def.flags = TF_READ_ONLY;
        def.permission_key = "read";
        SchemaBuilder b;
        b.str("path", "каталог; пусто = корень проекта")
         .integer_range("k", "максимум записей (по умолчанию 100)", 1, 100000);
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            std::string dir = arg_str(a, "path");
            if (dir.empty()) dir = ctx.project_dir();
            if (dir.empty()) return out("[ошибка] пустой path — укажи каталог");
            if (!fs::exists(dir) || !fs::is_directory(dir))
                return out("[ошибка] каталог не существует: " + dir);
            std::string perm = guard_permission(ctx, dir);
            if (!perm.empty()) return out(std::move(perm));
            size_t limit = (arg_int(a, "k") > 0) ? static_cast<size_t>(arg_int(a, "k")) : 100;
            std::stringstream out_s;
            out_s << "[list_dir] " << dir << ":\n";
            std::error_code ec;
            size_t count = 0;
            for (auto it = fs::directory_iterator(dir, ec);
                 it != fs::directory_iterator(); it.increment(ec)) {
                if (ec) break;
                const auto& p = it->path();
                std::string name = p.filename().string();
                if (it->is_directory()) name += "/";
                out_s << name << "\n";
                if (++count >= limit) {
                    out_s << "...[лимит " << limit << " записей]";
                    break;
                }
            }
            return out("list " + dir, out_s.str());
        };
        reg.register_def(std::move(def));
    }

    /* 3.2 web_fetch: HTTP GET через curl с лимитом вывода. */
    {
        ToolDef def;
        def.name = "web_fetch";
        def.description = "HTTP GET запрос к URL (через curl)";
        /* Сеть разрешена даже в research: это чтение, а не изменение. */
        def.flags = TF_READ_ONLY | TF_NETWORK;
        def.permission_key = "read";
        SchemaBuilder b;
        b.str("url", "адрес http(s)")
         .required("url");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            std::string url = arg_str(a, "url");
            if (url.empty()) return out("[ошибка] пустой url");
            std::string cmd = "curl -s -L -m 30 --max-redirs 3 " + shell::shell_quote(url);
            std::string result = shell::run_capture(cmd, 40);
            if (result.empty()) return out("web_fetch", "[web_fetch: пустой ответ]");
            return out("web_fetch", shell::cap(result, kMaxToolOutput));
        };
        reg.register_def(std::move(def));
    }

    /* 3.3 edit_file: замена диапазона строк.
     * k: строка начала (1-based), query: строка конца (1-based, включительно,
     * пусто = только k), content: новый текст строк. */
    {
        ToolDef def;
        def.name = "edit_file";
        def.description = "Замена диапазона строк файла";
        def.flags = TF_WRITES_FILES;
        def.permission_key = "write";
        SchemaBuilder b;
        b.str("path", "путь к файлу")
         .integer_range("k", "номер первой заменяемой строки (с 1)", 1, 10000000)
         .str("query", "номер последней заменяемой строки; пусто = только k")
         .str("content", "новые строки")
         .required("path").required("k").required("content");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            const std::string rel = arg_str(a, "path");
            const std::string content_in = arg_str(a, "content");
            int k = arg_int(a, "k");
            std::string abs = project_resolve(rel);
            if (k <= 0) return out("[ошибка] укажи k: номер строки начала (1-based)");
            std::ifstream fin(abs, std::ios::binary);
            if (!fin) return out("[ошибка] не удалось открыть файл: " + abs);
            std::vector<std::string> lines;
            std::string ln;
            while (std::getline(fin, ln)) lines.push_back(ln);
            fin.close();
            if (lines.empty()) return out("[ошибка] пустой файл: " + abs);

            size_t start = static_cast<size_t>(k);
            size_t end = start;
            std::string end_arg = arg_str(a, "query");
            if (!end_arg.empty()) {
                try { end = static_cast<size_t>(std::stoul(end_arg)); }
                catch (...) { return out("[ошибка] невалидный query (строка конца): " + end_arg); }
            }
            if (start < 1 || start > lines.size() || end < start || end > lines.size())
                return out("[ошибка] диапазон строк вне файла: k=" + std::to_string(k)
                           + " query=" + end_arg + " (всего строк: "
                           + std::to_string(lines.size()) + ")");

            /* Замена: start..end (1-based, включительно) на content. */
            std::vector<std::string> replacement;
            std::string rline;
            std::istringstream iss(content_in);
            while (std::getline(iss, rline)) replacement.push_back(rline);

            std::vector<std::string> result;
            result.reserve(lines.size() + replacement.size());
            for (size_t i = 0; i < start - 1; ++i) result.push_back(std::move(lines[i]));
            for (auto& r : replacement) result.push_back(std::move(r));
            for (size_t i = end; i < lines.size(); ++i) result.push_back(std::move(lines[i]));

            std::string new_content;
            for (const auto& l : result) { new_content += l; new_content += '\n'; }

            if (ctx.propose_write(rel, new_content)) {
                return out("предложено " + rel, "[предложено] " + abs + " (edit_file, строки "
                           + std::to_string(start) + "-" + std::to_string(end) + ")");
            }

            std::string perm = guard_permission(ctx, abs);
            if (!perm.empty()) return out(std::move(perm));
            backup_file(abs);  // для undo_edit (3.5)
            std::ofstream fout(abs, std::ios::binary | std::ios::trunc);
            if (!fout) return out("[ошибка] не удалось записать: " + abs);
            fout << new_content;
            fout.close();
            return out("правка " + rel, "[edit_file] " + abs + ": заменены строки "
                       + std::to_string(start) + "-" + std::to_string(end));
        };
        reg.register_def(std::move(def));
    }

    /* 3.5 undo_edit: отмена последней правки из .orig-backup. */
    {
        ToolDef def;
        def.name = "undo_edit";
        def.description = "Отмена последней правки файла (из резервной копии .orig)";
        def.flags = TF_WRITES_FILES;
        def.permission_key = "write";
        SchemaBuilder b;
        b.str("path", "путь к файлу").required("path");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            const std::string rel = arg_str(a, "path");
            std::string abs = project_resolve(rel);
            std::string bak = abs + ".orig";
            if (!fs::exists(bak)) return out("[ошибка] нет backup (.orig) для " + abs);
            /* Откат — это тоже запись, поэтому в режиме плана он
             * предлагается, а не выполняется. */
            std::error_code ec;
            std::ifstream restore(bak, std::ios::binary);
            if (!restore) return out("[ошибка] не удалось прочитать backup: " + bak);
            std::string restored((std::istreambuf_iterator<char>(restore)),
                                 std::istreambuf_iterator<char>());
            restore.close();
            if (ctx.propose_write(rel, restored)) {
                return out("предложено откат " + rel, "[предложено] откат " + abs);
            }
            std::string perm = guard_permission(ctx, abs);
            if (!perm.empty()) return out(std::move(perm));
            fs::copy_file(bak, abs, fs::copy_options::overwrite_existing, ec);
            if (ec) return out("[ошибка] восстановление не удалось: " + ec.message());
            return out("откат " + rel, "[undo_edit] восстановлен файл: " + abs);
        };
        reg.register_def(std::move(def));
    }
}

void register_rag_tools() {
    auto& reg = ToolsRegistry::instance();

    {
        ToolDef def;
        def.name = "rag_index";
        def.description = "Индексация PHP-файлов проекта в RAG";
        /* Перестраивает индекс RAG — внешнее хранилище, откатить
         * инструментом нельзя. */
        def.flags = TF_WRITES_FILES | TF_DESTRUCTIVE | TF_SLOW;
        def.permission_key = "rag";
        SchemaBuilder b;
        b.str("root", "каталог; пусто = корень проекта");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            std::string base = arg_str(a, "root");
            if (base.empty()) base = ctx.project_dir();
            if (base.empty()) return out("[ошибка] не задан корень индексации");
            std::vector<std::string> files;
            file_utils::walk_files(base, files, 4000, kSkipDirs, ".php");
            const auto& cb = ctx.callbacks();
            int ok = 0;
            for (const auto& fp : files) {
                if (cb.rag_process_document && cb.rag_process_document(fp)) ++ok;
            }
            std::stringstream s;
            s << "[проиндексировано " << ok << "/" << files.size() << " php-файлов в RAG]";
            return out("rag_index", s.str());
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "rag_query";
        def.description = "Поиск по индексу RAG";
        def.flags = TF_READ_ONLY;
        def.permission_key = "read";
        SchemaBuilder b;
        b.str("query", "поисковый запрос")
         .integer_range("k", "сколько фрагментов вернуть", 1, 50)
         .required("query");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            const auto& cb = ctx.callbacks();
            if (!cb.rag_build_prompt) return out("[ошибка] RAG не доступен");
            std::string result = cb.rag_build_prompt(arg_str(a, "query"), arg_int(a, "k"), "");
            if (result.empty()) return out("rag_query", "[RAG: пусто — проект не проиндексирован]");
            return out("rag_query", shell::cap(result, kMaxToolOutput));
        };
        reg.register_def(std::move(def));
    }
}

} // namespace coder
