#include "base_tools.h"
#include "tool.h"
#include "tools_registry.h"
#include "engine.h"
#include "project.h"
#include "security.h"
#include "shell.h"
#include "limits.h"
#include "file_utils.h"
#include "glob.h"
#include "instruction.h"
#include "apply_patch.h"
#include "text_edit.h"
#include "diff.h"
#include "file_lock.h"
#include "json_utils.h"
#include "snapshot.h"
#include "subagent.h"

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

/* И4.5: вернуть содержимое в формате существующего файла.
 *
 * Только для полной перезаписи: если файла нет, пишем как есть. Если
 * файл существует и у него CRLF или BOM, приводим новое содержимое к
 * тому же виду — иначе правка выглядит как перезапись всего файла. */
std::string preserve_file_format(const std::string& abs,
                                 const std::string& content) {
    std::error_code ec;
    if (!fs::exists(abs, ec)) return content;
    std::ifstream f(abs, std::ios::binary);
    if (!f) return content;
    const std::string head((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    f.close();
    const TextFile old = split_text(head);
    if (old.bom.empty() && !old.crlf) return content;

    TextFile nf = split_text(content);
    nf.bom = old.bom;
    nf.crlf = old.crlf;
    return join_text(nf);
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

/* skill_detail: полный текст навыка по имени.
 *
 * И9.5: тело отдаётся в обёртке <skill_content name="…">, а рядом —
 * <skill_files> с тем, что лежит в каталоге навыка. Обе метки названы в
 * kBaseSystemPrompt: строка формата, о которой модель не знает, ею и не
 * пользуется (общее правило файла, D2). До И9.5 здесь был голый
 * заголовок «### НАВЫК: …» с телом, и модель не могла отличить текст
 * навыка от текста инструмента.
 *
 * Пустой <skill_files> НЕ печатается: пустой раздел выглядит как
 * «ресурсов нет, а список пуст», то есть как отсутствие ресурсов там, где
 * проверка их просто не делала. */
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
    std::stringstream out;
    out << "<skill_content name=\"" << sk->name << "\">\n";
    if (sk->body.empty()) {
        out << "[у навыка нет подробной инструкции — доступно только "
               "описание из каталога]";
    } else {
        out << sk->body;
    }
    if (!sk->files.empty()) {
        out << "\n<skill_files>\n";
        for (const auto& f : sk->files) out << "- " << f << "\n";
        out << "</skill_files>";
    }
    out << "\n</skill_content>";
    return out.str();
}

/* --- И4.6: план задачи (todowrite / toread) --- */

/* Сравнение без приведения регистра. Наивный tolower здесь не годится:
 * в локали «C» он не трогает кириллицу, и «ВЫПОЛНЕНО» не совпало бы с
 * «выполнено» — список молча рассыпался бы. Модели пишут либо строчными,
 * либо прописными, поэтому сравниваем с обоими написаниями слова. */
/* Прописная форма слова: ASCII плюс кириллица.
 *
 * Диапазоны кириллицы в UTF-8 переходят МЕЖДУ ведущими байтами: «ы…ь»
 * лежат в D1 80..8F, а «Ы…Ь» — в D0 A0..AF. Поэтому «сдвинуть второй
 * байт на 0x20» нельзя (так не работало, и «ВЫПОЛНЕНО» не совпадало с
 * «выполнено»), и нужен разбор по четырём диапазонам. */
std::string upper_form(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 'a' && c <= 'z') {
            r += static_cast<char>(c - 'a' + 'A');
            continue;
        }
        if (c == 0xD0 || c == 0xD1) {
            if (i + 1 >= s.size()) { r += static_cast<char>(c); continue; }
            const unsigned char n = static_cast<unsigned char>(s[i + 1]);
            if (c == 0xD0 && n >= 0xA0 && n <= 0xAF) {
                r += static_cast<char>(0xD0);
                r += static_cast<char>(n - 0x20);
                ++i;
                continue;
            }
            if (c == 0xD0 && n >= 0xB0 && n <= 0xBF) {
                r += static_cast<char>(0xD0);
                r += static_cast<char>(n - 0x20);
                ++i;
                continue;
            }
            if (c == 0xD1 && n >= 0x80 && n <= 0x8F) {
                r += static_cast<char>(0xD0);
                r += static_cast<char>(n + 0x20);
                ++i;
                continue;
            }
            if (c == 0xD1 && n >= 0x90 && n <= 0x9F) {
                r += static_cast<char>(0xD1);
                r += static_cast<char>(n - 0x20);
                ++i;
                continue;
            }
        }
        r += static_cast<char>(c);
    }
    return r;
}

bool eq_ci(const std::string& raw, const char* word) {
    return raw == word || raw == upper_form(word);
}

std::string normalize_status(const std::string& raw, bool& known) {
    static const char* kPending[] = {"", "pending", "new", "todo",
                                     "не начат", "не начато", "ожидает"};
    static const char* kDoing[] = {"in_progress", "in progress", "doing",
                                    "active", "started", "в работе", "делаю",
                                    "начато"};
    static const char* kDone[] = {"completed", "complete", "done", "finished",
                                  "готово", "сделано", "выполнено"};
    static const char* kCancelled[] = {"cancelled", "canceled", "skipped",
                                       "отменено", "пропущено"};
    for (const char* w : kPending) if (eq_ci(raw, w)) { known = true; return "pending"; }
    for (const char* w : kDoing) if (eq_ci(raw, w)) { known = true; return "in_progress"; }
    for (const char* w : kDone) if (eq_ci(raw, w)) { known = true; return "completed"; }
    for (const char* w : kCancelled) if (eq_ci(raw, w)) { known = true; return "cancelled"; }
    known = false;
    return "pending";
}

std::string normalize_priority(const std::string& raw, bool& known) {
    static const char* kMedium[] = {"", "medium", "normal", "обычный", "средний"};
    static const char* kHigh[] = {"high", "срочно", "высокий", "важный"};
    static const char* kLow[] = {"low", "низкий", "неважный"};
    for (const char* w : kMedium) if (eq_ci(raw, w)) { known = true; return "medium"; }
    for (const char* w : kHigh) if (eq_ci(raw, w)) { known = true; return "high"; }
    for (const char* w : kLow) if (eq_ci(raw, w)) { known = true; return "low"; }
    known = false;
    return "medium";
}

std::string render_todos(const std::vector<TodoItem>& todos) {
    std::stringstream s;
    if (todos.empty()) return "[todowrite] план пуст";
    s << "[todowrite] план из " << todos.size() << " пунктов:\n";
    for (const auto& t : todos) {
        s << "  [" << t.status << "] " << t.id << ". " << t.content;
        if (t.priority != "medium") s << " (" << t.priority << ")";
        s << "\n";
    }
    return s.str();
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

/* И11.2: diff «до/после» в метаданных результата вызова.
 *
 * Форму собирает core (`diff::filediff_metadata`) — одна на все пишущие
 * инструменты; здесь только пристегнуть её к ToolOutput. Существующие
 * метаданные НЕ затираются: у `apply_patch` в них свои счётчики и список
 * файлов, и появление diff не должно стирать их (объект создаётся, только
 * если его не было).
 *
 * Кто вызывает: инструменты, которые реально записали (или предложили
 * записать) файл. Отказ и запрет diff не несут: ничего не изменилось, и
 * показывать нечего. */
ToolOutput with_filediff(ToolOutput o,
                         const std::vector<diff::FileDiff>& files) {
    if (files.empty()) return o;
    if (!o.metadata.is_object()) o.metadata = json::JsonValue::object();
    o.metadata.set("filediff", diff::filediff_metadata(files));
    return o;
}

/* И11.2: прочитать содержимое файла ДО записи — «до» для diff. Отсутствие
 * файла и ошибка чтения дают пустое содержимое, а не отказ: файл, который
 * нечем прочитать, инструмент запишет иначе (например, создаст), и пустая
 * «до» тогда честна, а отказ здесь означал бы, что правку не показали бы
 * вовсе. */
std::string read_existing(const std::string& abs) {
    std::ifstream fin(abs, std::ios::binary);
    if (!fin) return std::string();
    return std::string((std::istreambuf_iterator<char>(fin)),
                       std::istreambuf_iterator<char>());
}

/* Список путей в ответ инструмента. Показывается начало, а не всё, и
 * число остальных названо: список без хвоста молчал бы, что вернулось
 * двадцать файлов, а вернулось семьдесят. Потолок — тот же, что у
 * сообщения об откате: список читают глазами, и он не должен съедать
 * контекст запроса. */
std::string file_list_lines(const std::vector<std::string>& files,
                            size_t max_lines = 40) {
    std::string text;
    const size_t shown = std::min(max_lines, files.size());
    for (size_t i = 0; i < shown; ++i) text += "  " + files[i] + "\n";
    if (files.size() > shown) {
        text += "  …и ещё " + std::to_string(files.size() - shown) + "\n";
    }
    return text;
}

/* Обрезка по краям пробелов и переводов строк: модель присылает хеш с
 * переводом каретки нередко, и такой хеш не совпал бы ни с одним
 * снимком — отказ был бы враньём («нет такого снимка» вместо «лишний
 * перевод строки»). */
std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/* И10.4: `undo` и `redo` — переходы по стеку уровней сессии.
 *
 * ОДИН конструктор на два инструмента, а не две копии тела: два способа
 * сказать одно и то же — это дрейф (Д2), а здесь совпадает всё, кроме
 * направления и слов в описании. Отличаются они могут только тем, что
 * undo идёт назад, а redo вперёд.
 *
 * Ключ разрешения — ТОТ ЖЕ, что у revert. Это решение, а не экономия:
 *   - «всегда разрешить отмену» должно означать отмену, а не отмену плюс
 *     обычную запись файлов (отклонение 119 — ровно поэтому у revert
 *     свой ключ, а не write);
 *   - отмена и возврат — одно и то же право человека на перезапись
 *     проекта, и спрашивать дважды об одном вопросе незачем;
 *   - агент-поиск запрещает ключ revert ЦЕЛИКОМ, и новые инструменты под
 *     тем же ключом скрылись у него сами, без правки его правил
 *     (проверка deny_whole_key в tests/test_agent_config.cpp).
 *
 * TF_DESTRUCTIVE — как у revert: содержимое файлов, которое человек
 * правил руками, перезаписывается (отклонение 120), поэтому в режиме
 * плана оба запрещены, а не предложены. Вопрос задаёт ToolRunner::run. */
static ToolDef make_history_move_tool(const char* name, bool is_undo) {
    ToolDef def;
    def.name = name;
    def.description =
        is_undo
            ? "Отменить работу агента: вернуть файлы проекта к состоянию, "
              "которое было ДО последнего шага, что-то изменившего. "
              "Повторный вызов отменяет предыдущий шаг — откат "
              "многоуровневый, а не на одну правку (стек уровней "
              "ограничен по глубине, и самые старые уровни забываются). "
              "Шаг, ничего не менявший, уровня не создаёт и не отменяется. "
              "Файлы, созданные после этого состояния, НЕ удаляются: "
              "откат возвращает содержимое, но не убирает лишнее. "
              "Вернуться вперёд можно инструментом redo, пока агент не "
              "сделал новых правок. Отдельный откат к состоянию, "
              "которое человек назвал хешем, — инструмент revert."
            : "Вернуть отменённое: вернуть файлы проекта к состоянию, "
              "которое было ПОСЛЕ отменённого шага. Работает, только пока "
              "агент не сделал новых правок: после них состояние, "
              "возвращённое вперёд, больше не лежит в стеке, и возврат "
              "был бы прыжком через сделанное. Файлы, созданные после "
              "возвращаемого состояния, НЕ удаляются.";
    def.flags = TF_WRITES_FILES | TF_EXECUTES | TF_DESTRUCTIVE | TF_SLOW;
    def.permission_key = "revert";
    def.parameters = SchemaBuilder().build();
    def.handler = [is_undo](const json::JsonValue&, ToolContext& ctx) -> ToolOutput {
        const std::string verb = is_undo ? "undo" : "redo";
        const std::string title = is_undo ? "отмена шага" : "возврат отменённого";
        const std::string project_dir = ctx.project_dir();
        const HostCallbacks& cbs = ctx.callbacks();
        const std::string store =
            snapshot::store_dir(cbs.path_data_dir ? cbs.path_data_dir() : "");

        /* Стек копируется под локом и кладётся обратно под локом, а всё
         * между ними (git, копирование каталогов) идёт без лока: держать
         * state_.mtx на них — это висящий GUI (D1, правило 3). Копия, а
         * не ссылка: единственный писатель стека — рабочий поток агента,
         * и UI его не касается (шапка core/snapshot.h). */
        snapshot::UndoStack stack;
        {
            std::lock_guard<std::mutex> lk(ctx.state().mtx);
            stack = ctx.state().undo_stack;
        }
        const snapshot::MoveResult r =
            is_undo ? stack.undo(project_dir, store)
                    : stack.redo(project_dir, store);
        std::vector<snapshot::Snapshot> trash;
        {
            std::lock_guard<std::mutex> lk(ctx.state().mtx);
            ctx.state().undo_stack = stack;
            trash = ctx.state().undo_stack.take_trash();
        }
        snapshot::discard_copies(trash);

        /* Положение в стеке — в каждом ответе, включая отказ: иначе модель,
         * получив «отменять нечего», не знает, где стоит, и начинает жать
         * снова или искать обход. */
        const std::string where =
            r.levels == 0
                ? std::string("уровней нет")
                : ("уровень " + std::to_string(r.level) + " из " +
                   std::to_string(r.levels));
        const std::string what_next =
            std::string("отменить ещё: ") + (r.can_undo ? "да" : "нет") +
            ", вернуть вперёд: " + (r.can_redo ? "да" : "нет");
        if (!r.ok) {
            return out(title + " не состоялся",
                       "[" + verb + "] " +
                           (is_undo ? "отменять нечего или не вышло: "
                                    : "возвращать нечего или не вышло: ") +
                           r.reason + " (" + where + "; " + what_next + ")");
        }

        std::string text = "[" + verb + "] состояние " + r.level_hash + " (" +
                           r.source + ") возвращено файлов: " +
                           std::to_string(r.restored.size()) + " (" + where +
                           ").";
        text += file_list_lines(r.restored);
        if (!r.leftover.empty()) {
            /* Механизм НЕ угадывается — как у revert: для копии каталога
             * это отказ записи, для git-пути такого быть не должно, и
             * выдуманная причина в ответе была бы враньём. */
            text += "Не удалось вернуть (" + std::to_string(r.leftover.size()) +
                    "): после перехода эти файлы всё ещё отличаются от "
                    "уровня.\n" + file_list_lines(r.leftover);
        }
        text +=
            "Файлы, которых в состоянии нет, не удалены: снимок не видит "
            "неотслеживаемые файлы, и откат их не трогает.";
        if (!r.can_redo && is_undo == false) {
            text += " Дальше вперёд идти некуда: возвращаться было не с чего.";
        }
        text += " " + what_next + ".";
        return out(title, std::move(text));
    };
    return def;
}

} // anonymous namespace

void register_base_tools() {
    auto& reg = ToolsRegistry::instance();

    /* 4.7 read_file: диапазон строк вместо «пропустить K строк», отказ
     * для двоичных файлов, список для каталога. */
    {
        ToolDef def;
        def.name = "read_file";
        def.description =
            "Чтение текстового файла. На каталог возвращает список"
            " содержимого. Двоичные файлы не читает.";
        def.flags = TF_READ_ONLY;
        def.permission_key = "read";
        SchemaBuilder b;
        b.str("path", "путь к файлу относительно корня проекта")
         .integer_range("offset", "с какой строки читать (1 — с первой;"
                        " 0 = с начала)", 0, 10000000)
         .integer_range("limit", "сколько строк вернуть (по умолчанию 2000)",
                        1, 1000000)
         .required("path");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            const std::string rel = arg_str(a, "path");
            const std::string abs = project_resolve(rel);
            std::string perm = guard_permission(ctx, abs);
            if (!perm.empty()) return out(std::move(perm));

            std::error_code ec;
            if (!fs::exists(abs, ec))
                return out("read " + rel,
                           "[ошибка] файла нет: " + abs +
                               ". Посмотри каталог: list или glob.");
            /* Каталог: список содержимого вместо отказа. Раньше чтение
             * каталога давало «не удалось открыть файл», и модель гадала,
             * существует ли он. */
            if (fs::is_directory(abs, ec)) {
                std::stringstream s;
                s << "[каталог] " << rel << ":\n";
                int n = 0;
                for (auto it = fs::directory_iterator(abs, ec);
                     it != fs::directory_iterator() && !ec; it.increment(ec)) {
                    std::string name = it->path().filename().string();
                    std::error_code ec2;
                    if (it->is_directory(ec2)) name += "/";
                    if (++n > 200) {
                        s << "...[больше 200 записей — используй glob]\n";
                        break;
                    }
                    s << "  " << name << "\n";
                }
                ToolOutput o = out("read " + rel, s.str());
                o.metadata = json::JsonValue::object();
                o.metadata.set("is_dir", true);
                return o;
            }

            /* Двоичный файл: доля непечатаемых символов (И4.7). Вываливать
             * его в контекст бессмысленно и дорого — модель получит мусор,
             * а следующий запрос станет в разы больше. */
            {
                std::ifstream probe(abs, std::ios::binary);
                std::string head(limits::kBinarySniffBytes, '\0');
                probe.read(&head[0], static_cast<std::streamsize>(head.size()));
                const std::streamsize got = probe.gcount();
                if (got > 0) {
                    size_t odd = 0;
                    for (std::streamsize i = 0; i < got; ++i) {
                        const unsigned char c = static_cast<unsigned char>(head[i]);
                        const bool printable = c == '\n' || c == '\r' || c == '\t' ||
                                               (c >= 0x20 && c != 0x7F);
                        if (!printable) ++odd;
                    }
                    if (static_cast<double>(odd) >
                        limits::kBinaryNonPrintableRatio * got) {
                        return out("read " + rel,
                                   "[двоичный файл] " + rel +
                                       " не является текстом (доля"
                                       " непечатаемых символов выше 30%)."
                                       " Прочитать его текстом нельзя; используй"
                                       " другой инструмент для этого формата.");
                    }
                }
            }

            const int offset_arg = arg_int(a, "offset");
            const size_t first_line =
                (offset_arg > 1) ? static_cast<size_t>(offset_arg) : 1;
            const int limit_arg = arg_int(a, "limit");
            const size_t limit = (limit_arg > 0)
                                     ? static_cast<size_t>(limit_arg)
                                     : limits::kReadDefaultLines;

            /* Читаем построчно и останавливаемся на limit или на лимите
             * байт: файл на сотни мегабайт не должен попадать в память
             * целиком ради двух строк. */
            std::ifstream f(abs, std::ios::binary);
            std::stringstream body;
            std::string ln;
            size_t line_no = 0, taken = 0, bytes = 0;
            bool byte_capped = false, more = false;
            while (std::getline(f, ln)) {
                ++line_no;
                if (line_no < first_line) continue;
                if (taken >= limit) { more = true; break; }
                if (bytes + ln.size() + 1 > kReadFileChars) {
                    byte_capped = true;
                    more = true;
                    break;
                }
                bytes += ln.size() + 1;
                ++taken;
                body << ln << "\n";
            }

            std::stringstream hdr;
            hdr << "# read: " << rel;
            if (first_line > 1) hdr << " (с строки " << first_line << ")";
            hdr << " [" << taken << " строк";
            if (line_no > 0) hdr << " из " << line_no;
            hdr << "]";
            if (more) {
                /* Слово «обрезано» — общее для всех инструментов: по нему
                 * и модель, и тест понимают, что вывод неполон. */
                hdr << (byte_capped
                            ? " — обрезано по лимиту байт, читай offset="
                            : " — обрезано по limit, дальше есть: читай offset=")
                    << (first_line + taken);
            }

            /* И9.2: инструкции рядом с прочитанным файлом. ОДИН обход
             * на двоих: и список для metadata.loaded (И4.7), и прикрепление
             * к промпту. Два обхода разошлись бы при первой же правке
             * (например, если правило перестанет подниматься к корню), и
             * модель получала бы в подписи один список, а в промпте другой.
             *
             * Прикрепление — отдельным шагом, а не частью чтения: правила
             * должны попасть в СЛЕДУЮЩИЙ запрос к модели (промпт собирается
             * там), и Engine::attach_instructions сбрасывает его кэш. */
            std::vector<Instruction> nearby =
                instruction::resolve(abs, ctx.project_dir());
            engine().attach_instructions(nearby);

            json::JsonValue loaded = json::JsonValue::array();
            for (const Instruction& in : nearby) {
                json::JsonValue e = json::JsonValue::object();
                e.set("path", in.label);
                loaded.push_back(std::move(e));
            }
            if (loaded.size() > 0) {
                std::stringstream il;
                il << " [рядом найдены инструкции проекта:";
                for (size_t i = 0; i < loaded.size(); ++i)
                    il << " " << loaded.at(i).get_string("path");
                il << "]";
                hdr << il.str();
            }

            ToolOutput o;
            o.title = "read " + rel;
            o.output =
                shell::cap(hdr.str() + "\n" + body.str(), limits::kMaxToolOutput + 2000);
            o.truncated = more;
            o.metadata = json::JsonValue::object();
            o.metadata.set("path", rel);
            o.metadata.set("lines", static_cast<long long>(taken));
            o.metadata.set("offset", static_cast<long long>(first_line));
            o.metadata.set("limit", static_cast<long long>(limit));
            o.metadata.set("more", more);
            o.metadata.set("loaded", std::move(loaded));
            return o;
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
            /* И4.5: правка файла — цикл «прочитал → изменил → записал»,
             * а писать могут два потока (агент и кнопка «применить» в UI).
             * Блокировка берётся ДО чтения и держится до конца записи. */
            file_lock::Guard file_guard(abs);
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
            /* И4.5: формат существующего файла сохраняется. Перезапись
             * CRLF-файла через LF молча ломает .gitattributes и
             * .bat/.sh, а BOM ждут Windows-редакторы и часть CI. */
            const std::string body = preserve_file_format(abs, content);
            /* И11.2: «до» читается под уже взятой блокировкой файла и до
             * записи, иначе читать было бы нечего. Сравнивается ровно то,
             * что уйдёт в файл (`body`, а не `content`): diff, посчитанный
             * не по тому, что записано, врал бы о числе правок. */
            const std::string before_raw = read_existing(abs);
            const diff::FileDiff fd = diff::make_file_diff(rel, before_raw, body);
            std::ofstream f(abs, std::ios::binary | std::ios::trunc);
            if (!f) return out("[ошибка] не удалось записать: " + abs);
            f << body;
            f.close();
            return with_filediff(
                out("записано " + rel, "[записано] " + abs + " (" +
                           std::to_string(body.size()) + " байт)"),
                {fd});
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

    /* 4.1 glob: поиск файлов по шаблону пути. */
    {
        ToolDef def;
        def.name = "glob";
        def.description =
            "Поиск файлов по шаблону пути: ** — любые каталоги, * и ? —"
            " внутри имени, {a,b} — альтернативы. Свежие файлы сверху.";
        def.flags = TF_READ_ONLY;
        def.permission_key = "read";
        SchemaBuilder b;
        b.str("pattern", "шаблон пути, например **/*.php или src/**/*.{ts,tsx}")
         .str("path", "каталог для поиска; пусто = корень проекта")
         .str("include", "фильтр по расширениям через запятую: php,js (пусто = любые)")
         .required("pattern");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            const std::string pattern = arg_str(a, "pattern");
            const std::string rel_root = arg_str(a, "path");
            /* Пустой path = корень проекта; относительный разрешается от
             * него же, а не от каталога процесса. */
            std::string root = rel_root.empty() ? ctx.project_dir()
                                                : project_resolve(rel_root);
            if (root.empty())
                return out("[ошибка] не задан path и не задан корень проекта");
            std::string perm = guard_permission(ctx, root);
            if (!perm.empty()) return out(std::move(perm));

            fileglob::Options opt;
            const std::string include = arg_str(a, "include");
            if (!include.empty()) {
                std::string cur;
                std::string list = include;
                list += ',';
                for (char c : list) {
                    if (c == ',' || c == ' ' || c == ';') {
                        if (!cur.empty()) opt.extensions.push_back(cur);
                        cur.clear();
                    } else {
                        cur += c;
                    }
                }
            }

            fileglob::Result r = fileglob::find(root, pattern, opt);
            if (!r.error.empty()) return out("glob", "[ошибка] " + r.error);

            std::stringstream s;
            s << "[glob] " << pattern << " в " << root << ": ";
            if (r.entries.empty()) {
                s << "ничего не найдено";
                if (r.walk_capped)
                    s << " [обход остановлен на " << r.visited << " записях —"
                         " ищи в каталоге поуже]";
                s << "\n";
            } else {
                s << r.entries.size();
                if (r.matched > r.entries.size())
                    s << " из " << r.matched << " (лимит " << opt.limit
                      << " — сузь шаблон или укажи path)";
                s << ", свежие сверху:\n";
                for (const auto& e : r.entries) s << e.path << "\n";
            }
            ToolOutput o;
            o.title = "glob " + pattern;
            o.output = shell::cap(s.str(), kMaxToolOutput);
            o.truncated = r.truncated || r.walk_capped;
            o.metadata = json::JsonValue::object();
            o.metadata.set("count", static_cast<long long>(r.entries.size()));
            o.metadata.set("matched", static_cast<long long>(r.matched));
            o.metadata.set("truncated", r.truncated);
            o.metadata.set("root", root);
            return o;
        };
        reg.register_def(std::move(def));
    }

    /* 4.6 todowrite: план задачи. */
    {
        ToolDef def;
        def.name = "todowrite";
        def.description =
            "Записать план задачи. Вызывай в начале сложной задачи и"
            " обновляй по ходу: пункт в работе — in_progress, сделанный —"
            " completed. План показывается пользователю, он им вмешивается.";
        /* Файлов не касается — план живёт в состоянии сессии, поэтому
         * доступен и в режиме Research. */
        def.flags = TF_READ_ONLY;
        def.permission_key = "todo";
        SchemaBuilder b;
        b.object_array("todos",
                       "массив пунктов: {id, content, status, priority}",
                       "id — номер пункта, content — текст, status — "
                       "pending|in_progress|completed|cancelled, priority — "
                       "low|medium|high")
         .required("todos");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            /* Тип массива уже проверен валидацией схемы (И1.6): сюда
             * не- массив не доходит, и своя проверка была бы мёртвым кодом. */
            const json::JsonValue& list = a.get("todos");
            if (list.size() == 0)
                return out("todowrite",
                           "[ошибка] пустой список пунктов. Если план закончен —"
                           " сообщи пользователю итог текстом, а не пустым"
                           " вызовом.");

            std::vector<TodoItem> items;
            std::string problems;
            int substituted = 0;
            for (size_t i = 0; i < list.size(); ++i) {
                const json::JsonValue& el = list.at(i);
                const std::string at = "пункт " + std::to_string(i + 1);
                if (!el.is_object()) {
                    problems += at + ": не объект (нужно {id, content, status,"
                                " priority}); ";
                    continue;
                }
                TodoItem t;
                t.content = el.get_string("content");
                if (t.content.empty()) {
                    problems += at + ": пустой content; ";
                    continue;
                }
                /* id модель пишет через раз, а ссылаться на пункт надо
                 * уметь всегда: проставляем по порядку. */
                t.id = el.get_string("id");
                if (t.id.empty()) t.id = std::to_string(i + 1);
                bool known = false;
                const std::string raw_status = el.get_string("status");
                t.status = normalize_status(raw_status, known);
                if (!known && !raw_status.empty()) {
                    ++substituted;
                    problems += at + ": статус '" + raw_status +
                                "' не распознан, взят pending; ";
                }
                const std::string raw_prio = el.get_string("priority");
                t.priority = normalize_priority(raw_prio, known);
                if (!known && !raw_prio.empty()) {
                    ++substituted;
                    problems += at + ": приоритет '" + raw_prio +
                                "' не распознан, взят medium; ";
                }
                items.push_back(std::move(t));
            }
            if (items.empty())
                return out("todowrite", "[ошибка] ни один пункт не разобран: "
                                       + problems);

            {
                std::lock_guard<std::mutex> lk(ctx.state().mtx);
                ctx.state().todos = items;
            }
            /* Кэш системного промпта: план печатается именно в нём, и без
             * сброса модель видела бы прошлый список ещё весь следующий ход. */
            engine().invalidate_prompt_cache();

            ToolOutput o;
            o.title = "todowrite (" + std::to_string(items.size()) + ")";
            std::string text = render_todos(items);
            if (!problems.empty())
                text += "[нормализовано: " + problems + "]\n";
            o.output = text;
            o.metadata = json::JsonValue::object();
            o.metadata.set("count", static_cast<long long>(items.size()));
            o.metadata.set("normalized", static_cast<long long>(substituted));
            return o;
        };
        reg.register_def(std::move(def));
    }

    /* 4.6 toread: текущий план задачи. */
    {
        ToolDef def;
        def.name = "todoread";
        def.description = "Показать текущий план задачи";
        def.flags = TF_READ_ONLY;
        def.permission_key = "todo";
        def.parameters = SchemaBuilder().build();
        def.handler = [](const json::JsonValue&, ToolContext& ctx) -> ToolOutput {
            std::vector<TodoItem> items;
            {
                std::lock_guard<std::mutex> lk(ctx.state().mtx);
                items = ctx.state().todos;
            }
            return out("todoread", render_todos(items));
        };
        reg.register_def(std::move(def));
    }

    /* 4.2 apply_patch: многофайловый патч в формате opencode. */
    {
        ToolDef def;
        def.name = "apply_patch";
        def.description =
            "Правка нескольких файлов одним патчем. Формат: *** Begin Patch,"
            " затем *** Add File: путь (строки с '+'), *** Update File: путь"
            " (хуки '@@', строки контекста с пробела, '-' удаляет, '+'"
            " добавляет, '*** Move to: путь' переименовывает, '*** End of"
            " File' — правка в конце файла), *** Delete File: путь, и"
            " *** End Patch.";
        def.flags = TF_WRITES_FILES;
        def.permission_key = "write";
        SchemaBuilder b;
        b.str("patchText", "текст патча между *** Begin Patch и *** End Patch")
         .required("patchText");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            const std::string text = arg_str(a, "patchText");
            const patch::Parsed p = patch::parse(text);
            if (!p.ok())
                return out("apply_patch", "[патч не разобран] " + p.error);
            /* И4.5: один семафор на весь патч — файлы патча могут
             * совпадать (переименование), и полосы по пути тогда не помогут. */
            file_lock::Guard patch_guard("<apply_patch>");

            /* Проверки путей и разрешений — ДО любой записи: патч из пяти
             * файлов не должен оставить систему наполовину изменённой. */
            std::vector<std::string> blocked;
            for (const auto& f : p.files) {
                const std::string rel = f.path;
                const std::string abs = project_resolve(rel);
                if (!security::is_path_safe(rel) ||
                    !security::is_path_not_dangerous(abs)) {
                    blocked.push_back(rel + " (небезопасный путь)");
                    continue;
                }
                if (f.op == patch::Op::Add) continue;  /* файла может не быть */
                if (!fs::exists(abs)) {
                    blocked.push_back(rel + " (файла нет — читай каталог: glob)");
                    continue;
                }
                const std::string perm = guard_permission(ctx, abs);
                if (!perm.empty()) {
                    blocked.push_back(rel + " (" + perm + ")");
                    continue;
                }
                if (!f.move_to.empty()) {
                    const std::string mv_abs = project_resolve(f.move_to);
                    const std::string perm2 = guard_permission(ctx, mv_abs);
                    if (!perm2.empty()) blocked.push_back(f.move_to + " (" + perm2 + ")");
                }
            }
            if (!blocked.empty()) {
                std::stringstream s;
                s << "[apply_patch: отказ, ничего не изменено]\n";
                for (const auto& b : blocked) s << "  " << b << "\n";
                return out("apply_patch", s.str());
            }

            /* Режим плана: предложить можно только запись содержимого.
             * Удаление и перемещение в ProposedWrite не выражаются, поэтому
             * они отказны, а не «предложены наполовину». */
            if (ctx.plan_mode()) {
                bool refused = false;
                for (const auto& f : p.files) {
                    if (f.op == patch::Op::Delete || !f.move_to.empty())
                        refused = true;
                }
                if (refused) {
                    return out("apply_patch",
                               "[режим «сначала план»: патч не применён]"
                               " Удаление и перемещение файлов предложить"
                               " нельзя — разбей задачу: сначала предложи"
                               " содержимое новых файлов через write_file.");
                }
            }

            std::stringstream report;
            int added = 0, updated = 0, deleted = 0, moved = 0, proposed = 0;
            json::JsonValue files_meta = json::JsonValue::array();
            /* И11.2: один патч меняет сколько угодно файлов, поэтому diff
             * копится и уходит одним списком — форма `filediff` одна и для
             * одного файла (граница 1 в шапке core/diff.h). */
            std::vector<diff::FileDiff> fds;

            /* Отказ посреди патча. К этому моменту часть файлов могла уже
             * быть записана, и молчать об этом нельзя: модель и пользователь
             * решили бы, что не тронуто ничего, и потеряли бы правки. */
            auto refuse = [&report, &fds](const std::string& what) {
                std::stringstream s;
                if (report.str().empty()) {
                    s << "[apply_patch: отказ, ничего не изменено]\n" << what;
                } else {
                    s << "[apply_patch: ОТКАЗ — патч применён НЕ полностью]\n"
                      << "Уже записано:\n"
                      << report.str()
                      << "Не выполнено: " << what << "\n"
                      << "Остальные файлы патча не тронуты. Перечитай их и"
                         " пришли оставшиеся операции отдельным патчем.";
                }
                /* И11.2: частично применённый патт — половина которого уже
                 * в проекте, — отдаёт diff того, что УЖЕ записано: текст
                 * отказа перечисляет файлы, а подсветка показывает правку. */
                return with_filediff(out("apply_patch", s.str()), fds);
            };

            for (const auto& f : p.files) {
                const std::string abs = project_resolve(f.path);

                if (f.op == patch::Op::Add) {
                    if (fs::exists(abs)) {
                        return refuse("файл уже существует: " + f.path +
                                      ". Для правки существующего файла"
                                      " используй '*** Update File:'");
                    }
                    std::string content;
                    for (const auto& h : f.hunks) {
                        for (const auto& l : h.new_lines) {
                            content += l;
                            content += '\n';
                        }
                    }
                    if (ctx.propose_write(f.path, content)) {
                        ++proposed;
                        report << "  предложено создать " << f.path << "\n";
                    } else {
                        /* Каталога может не быть: агент создаёт файлы в
                         * новых подкаталогах постоянно, и отказ «не удалось
                         * создать» без указания причины их бы обескуражил. */
                        std::error_code ec;
                        fs::create_directories(fs::path(abs).parent_path(), ec);
                        std::ofstream fo(abs, std::ios::binary | std::ios::trunc);
                        if (!fo) return refuse("не удалось создать: " + abs);
                        fo << content;
                        ++added;
                        report << "  создан " << f.path << " ("
                               << content.size() << " байт)\n";
                    }
                    /* И11.2: у нового файла «до» нет вовсе — это и есть
                     * создание, и виджет покажет сплошное добавление. */
                    fds.push_back(diff::make_file_diff(f.path, "", content));
                } else if (f.op == patch::Op::Delete) {
                    /* И11.2: содержимое читается ДО удаления — после
                     * удаления читать нечего, и удалённый файл остался бы
                     * без единой строки в панели. */
                    const std::string was = read_existing(abs);
                    std::error_code ec;
                    fs::remove(abs, ec);
                    if (ec)
                        return refuse("не удалось удалить " + f.path + ": "
                                      + ec.message());
                    ++deleted;
                    report << "  удалён " << f.path << "\n";
                    diff::FileDiff fd = diff::make_file_diff(f.path, was, "");
                    if (fd.additions == 0 && fd.deletions == 0) {
                        /* Пустой удалённый файл сравнить не с чем: без
                         * этой оговорки он выглядел бы как «содержимое
                         * совпадает», то есть как правки, которой не было. */
                        fd.note = "файл удалён";
                    }
                    fds.push_back(std::move(fd));
                } else {
                    std::ifstream fi(abs, std::ios::binary);
                    if (!fi) return refuse("не удалось прочитать: " + abs);
                    const std::string raw((std::istreambuf_iterator<char>(fi)),
                                          std::istreambuf_iterator<char>());
                    const patch::Applied ap =
                        patch::apply_hunks(split_text(raw), f.hunks);
                    if (!ap.ok) {
                        return refuse(f.path + ": хук #"
                                      + std::to_string(ap.failed_hunk + 1)
                                      + " не применён. " + ap.error);
                    }
                    const std::string result = join_text(ap.file);
                    if (ctx.propose_write(f.path, result)) {
                        ++proposed;
                        report << "  предложено изменить " << f.path << "\n";
                    } else {
                        std::ofstream fo(abs, std::ios::binary | std::ios::trunc);
                        if (!fo) return refuse("не удалось записать: " + abs);
                        fo << result;
                        ++updated;
                        report << "  изменён " << f.path;
                        if (!f.move_to.empty()) {
                            std::error_code ec;
                            const std::string mv_abs = project_resolve(f.move_to);
                            if (fs::exists(mv_abs)) {
                                return refuse("цель перемещения уже существует: "
                                              + f.move_to);
                            }
                            fs::rename(abs, mv_abs, ec);
                            if (ec)
                                return refuse("не удалось переместить в "
                                              + f.move_to + ": " + ec.message());
                            ++moved;
                            report << " → " << f.move_to;
                        }
                        report << "\n";
                    }
                    /* И11.2: имя файла в diff — ТО, по которому правка
                     * теперь лежит. При перемещении это новый путь: подписать
                     * правку старым именем значило бы показать изменение
                     * файла, которого по этому имени уже нет. */
                    fds.push_back(diff::make_file_diff(
                        f.move_to.empty() ? f.path : f.move_to, raw, result));
                }
                json::JsonValue meta = json::JsonValue::object();
                meta.set("path", f.path);
                meta.set("op", f.op == patch::Op::Add   ? "add"
                                : f.op == patch::Op::Delete ? "delete"
                                                            : "update");
                files_meta.push_back(std::move(meta));
            }

            std::stringstream head;
            head << "[apply_patch] готово: создано " << added << ", изменено "
                 << updated << ", удалено " << deleted;
            if (moved) head << ", перемещено " << moved;
            if (proposed) head << ", предложено " << proposed;
            head << "\n" << report.str();

            ToolOutput o;
            o.title = "apply_patch";
            o.output = shell::cap(head.str(), kMaxToolOutput);
            o.metadata = json::JsonValue::object();
            o.metadata.set("added", static_cast<long long>(added));
            o.metadata.set("updated", static_cast<long long>(updated));
            o.metadata.set("deleted", static_cast<long long>(deleted));
            o.metadata.set("moved", static_cast<long long>(moved));
            o.metadata.set("proposed", static_cast<long long>(proposed));
            o.metadata.set("files", std::move(files_meta));
            /* И11.2: diff идёт в те же метаданные, а не заменяет их: свои
             * счётчики у патча свои, и стирать их из-за чужого поля нельзя. */
            return with_filediff(std::move(o), fds);
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
        /* И9.6: двухуровневый каталог навыков. Подробный (имя + описание)
         * живёт в системном промпте, а здесь — КРАТКИЙ, только имена: описание
         * инструмента уходит в каждый запрос, и платить за пересказ всех
         * навыков на каждом шаге незачем, когда модель всё равно читает
         * подробный список выше. Заодно это единственное место, где имена
         * навыков известны модели БЕЗ вызова инструмента: список в
         * системном промпте меняется вместе с навыками, а описание
         * инструмента пересобирается при каждом вызове build_tool_catalogue.
         *
         * Динамическое описание ЗАМЕЩАЕТ статическое (И8.13) — поэтому само
         * «Полный текст навыка по имени» входит в dynamic, а не теряется. */
        def.description = "Полный текст навыка по имени";
        def.describe_dynamic = []() {
            return skill_detail_description(
                SkillsManager::instance().all_skills());
        };
        def.flags = TF_READ_ONLY;
        def.permission_key = "read";
        SchemaBuilder b;
        b.str("query", "имя навыка из списка в описании инструмента или из "
                       "каталога навыков в системном промпте").required("query");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            return out("навык", base_skill_detail(arg_str(a, "query")));
        };
        reg.register_def(std::move(def));
    }

    /* search_replace: поиск и замена текста в файле (diff-based edit). */
    /* 4.3 search_replace: замена текста через каскад из девяти стадий
     * (core/text_edit.h). Раньше требовалось буквальное совпадение, и
     * модель на каждой неудаче гадала, чего именно не хватило. */
    {
        ToolDef def;
        def.name = "search_replace";
        def.description =
            "Поиск и замена текста в файле. Совпадение не обязано быть"
            " буквальным: отступы, пробелы в концах строк и лишние"
            " переводы строк прощаются. Если фрагмент встречается"
            " несколько раз, добавь контекст или поставь replace_all.";
        def.flags = TF_WRITES_FILES;
        def.permission_key = "write";
        SchemaBuilder b;
        b.str("path", "путь к файлу")
         .str("query", "что искать")
         .str("content", "на что заменить")
         .boolean("replace_all", "заменить ВСЕ вхождения, а не одно")
         .required("path").required("query").required("content");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            const std::string rel = arg_str(a, "path");
            const std::string abs = project_resolve(rel);
            file_lock::Guard file_guard(abs);
            std::ifstream fin(abs, std::ios::binary);
            if (!fin) return out("[ошибка] не удалось открыть файл: " + abs);
            const std::string raw((std::istreambuf_iterator<char>(fin)),
                                  std::istreambuf_iterator<char>());

            EditRequest req;
            req.old_text = arg_str(a, "query");
            req.new_text = arg_str(a, "content");
            req.replace_all = a.get_bool("replace_all", false);
            if (req.old_text.empty())
                return out("[ошибка] пустой поисковый запрос (query)");

            const EditResult r = apply_edit(split_text(raw), req);
            if (!r.ok) return out("замена в " + rel, "[search_replace] " + r.error);
            const std::string result = join_text(r.file);
            /* И11.2: обе стороны уже в руках — прочитанное `raw` и то, что
             * уйдёт в файл (`result`). Пересчёт по патчу не нужен, форма
             * та же, что у остальных инструментов. */
            const diff::FileDiff fd = diff::make_file_diff(rel, raw, result);

            if (ctx.propose_write(rel, result)) {
                return with_filediff(
                    out("предложено " + rel,
                        "[предложено] " + abs + " (search_replace, строка "
                            + std::to_string(r.line) + ", стадия "
                            + edit_strategy_name(r.used) + ")"),
                    {fd});
            }

            std::string perm = guard_permission(ctx, abs);
            if (!perm.empty()) return out(std::move(perm));

            std::ofstream fout(abs, std::ios::binary | std::ios::trunc);
            if (!fout) return out("[ошибка] не удалось записать: " + abs);
            fout << result;
            fout.close();
            std::stringstream rep;
            rep << "[search_replace] " << abs << ": строка " << r.line
                << ", заменено " << r.occurrences << " вхождени(й)"
                << " (" << req.old_text.size() << " -> " << req.new_text.size()
                << " байт, стадия " << edit_strategy_name(r.used) << ")";
            if (r.used == EditStrategy::BlockAnchor) {
                /* Единственная нечёткая стадия: пользователь обязан знать,
                 * что правка попала по сходству, а не по точному тексту. */
                rep << " [ВНИМАНИЕ: совпадение нечёткое, проверь результат!]";
            }
            return with_filediff(out("замена в " + rel, rep.str()), {fd});
        };
        reg.register_def(std::move(def));
    }

    /* 4.8 bash: запуск shell-команды (заменил exec_command).
     *
     * Отличия от прежнего exec_command: таймаут 120 с по умолчанию вместо
     * 60, кольцевой буфер вместо первых 12 КБ, полный вывод уходит в файл
     * (spill) и его путь возвращается модели, а UI раз в секунду видит
     * признак жизни команды. Причина одна: у длинных команд хвост вывода —
     * это и есть ошибка, а раньше он как раз терялся. */
    {
        ToolDef def;
        def.name = "bash";
        def.description =
            "Запуск shell-команды. КРАЙНЯЯ мера: сначала пробуй"
            " специализированные инструменты. Таймаут по умолчанию 120 с;"
            " полный вывод длинной команды сохраняется в файл, путь"
            " возвращается в ответе.";
        /* DESTRUCTIVE: команда может сделать что угодно, и откатить это
         * инструмент не умеет. Именно поэтому он обязан быть виден
         * политике режимов и подтверждению пользователя. */
        def.flags = TF_EXECUTES | TF_DESTRUCTIVE | TF_SLOW;
        def.permission_key = "bash";
        SchemaBuilder b;
        b.str("cli", "команда для выполнения в shell")
         .integer_range("timeout", "таймаут в секундах (по умолчанию 120)",
                        1, 3600)
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
                std::string denial = guard_permission(ctx, outside);
                if (!denial.empty()) return out(std::move(denial));
            }

            shell::RunOptions opt;
            const int t = arg_int(a, "timeout");
            opt.timeout_sec = (t > 0) ? static_cast<unsigned>(t)
                                       : limits::kShellTimeoutSec;
            const std::string data_dir = ctx.callbacks().path_data_dir
                                             ? ctx.callbacks().path_data_dir()
                                             : "";
            opt.spill_path = shell::next_spill_path(data_dir, "bash");
            /* И6.7: «стоп» обязан убивать команду, а не только ожидание. Без
             * этого агент уходил на следующий шаг, а команда в фоне
             * продолжала работать и писать файлы. */
            if (AbortToken* token = ctx.abort()) {
                opt.should_cancel = [token]() { return token->aborted(); };
            }
            /* Живой признак жизни: раз в секунду в ленту приложения.
             * Без него «docker build» две минуты выглядит как зависание,
             * и пользователь жмёт «стоп» на живом процессе. */
            if (ctx.callbacks().chat_event) {
                opt.on_progress = [&ctx](size_t bytes, const std::string& tail) {
                    ctx.callbacks().chat_event(
                        "[bash] выполняется, " + std::to_string(bytes) +
                        " байт вывода" + (tail.empty() ? "" : ": " + tail));
                };
            }

            shell::RunStats stats;
            std::string result = shell::run_capture_stream(cmd, opt, stats);
            if (!result.empty() && result.rfind("[запрещено политикой", 0) == 0)
                return out("shell", result);

            std::stringstream meta;
            meta << "<shell_metadata> exit=" << stats.exit_code
                 << " bytes=" << stats.total_bytes
                 << " ms=" << static_cast<long long>(stats.elapsed_ms);
            if (stats.timed_out) {
                meta << " timed_out=true\n";
            }
            /* Отмена видна модели отдельным флагом: exit_code после SIGTERM
             * (143 или -1) не позволяет отличить «пользователь нажал стоп»
             * от «команда упала сама», а разница важна — в первом случае
             * повторять команду бессмысленно, во втором разбирать ошибку. */
            if (stats.cancelled) {
                meta << " cancelled=true\n";
            }
            if (stats.dropped_bytes > 0) {
                meta << " shown_from=" << stats.dropped_bytes << " (хвост)\n";
            }
            if (!stats.spill_path.empty()) {
                meta << " output_path=" << stats.spill_path << "\n";
            } else if (stats.dropped_bytes > 0) {
                meta << " output_path=недоступен (нет каталога данных)\n";
            }
            if (stats.timed_out) {
                /* Пояснение в терминах <shell_metadata>, как в opencode:
                 * модель должна понять, что произошло, из ответа, а не
                 * догадываться по обрыву текста. */
                meta << "Команда прервана по таймауту " << opt.timeout_sec
                     << " с. Показан хвост вывода";
                if (stats.dropped_bytes > 0)
                    meta << " (начало утрачено: " << stats.dropped_bytes << " байт)";
                meta << ".";
                if (!stats.spill_path.empty())
                    meta << " Полный вывод: " << stats.spill_path;
                meta << " Если команда законно долгая — повтори с"
                        " большим timeout.";
            }
            meta << "\n</shell_metadata>";

            ToolOutput o;
            o.title = "shell";
            /* Кольцо в run_capture_stream УЖЕ ограничило вывод, и второй
             * раз резать его нельзя: shell::cap берёт начало, то есть
             * отрезал бы хвост — ровно то, ради чего кольцо затевалось. */
            o.output = (result.empty() ? "[bash: нет вывода]" : result) +
                       (result.empty() ? "" : "\n") + meta.str();
            o.truncated = stats.dropped_bytes > 0;
            o.metadata = json::JsonValue::object();
            o.metadata.set("exit_code", static_cast<long long>(stats.exit_code));
            o.metadata.set("bytes", static_cast<long long>(stats.total_bytes));
            o.metadata.set("elapsed_ms", static_cast<long long>(stats.elapsed_ms));
            o.metadata.set("timed_out", stats.timed_out);
            o.metadata.set("truncated", stats.dropped_bytes > 0);
            o.metadata.set("output_path", stats.spill_path);
            return o;
        };
        reg.register_def(std::move(def));
    }

    /* 4.9 list: лёгкий список файлов и каталогов с глубиной.
     *
     * Заменяет list_dir (тот жил один уровень). Отдельный инструмент
     * list_dir рядом с list был бы двумя инструментами с одним смыслом —
     * ровно та двойственность, которую И1 убрала из реестра. */
    {
        ToolDef def;
        def.name = "list";
        def.description =
            "Список файлов и каталогов с заданной глубиной (каталоги"
            " помечены /). Для поиска по шаблону имени используй glob.";
        def.flags = TF_READ_ONLY;
        def.permission_key = "read";
        SchemaBuilder b;
        b.str("path", "каталог; пусто = корень проекта")
         .integer_range("depth", "глубина обхода: 1 = только этот каталог,"
                        " 2 = один уровень вглубь (по умолчанию 1)", 1, 10);
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            const std::string rel = arg_str(a, "path");
            const std::string root = rel.empty() ? ctx.project_dir()
                                                 : project_resolve(rel);
            if (root.empty())
                return out("[ошибка] не задан path и не задан корень проекта");
            std::string perm = guard_permission(ctx, root);
            if (!perm.empty()) return out(std::move(perm));

            int depth_arg = arg_int(a, "depth");
            const size_t depth = (depth_arg > 0) ? static_cast<size_t>(depth_arg) : 1;

            fileglob::Options opt;
            opt.include_dirs = true;
            opt.max_depth = depth;
            opt.newest_first = false;   /* список читают сверху вниз */
            opt.limit = limits::kListLimit;
            const fileglob::Result r = fileglob::find(root, "**", opt);
            if (!r.error.empty()) return out("list", "[ошибка] " + r.error);

            std::stringstream s;
            s << "[list] " << (rel.empty() ? std::string("") : rel + " ");
            s << root << " (глубина " << depth << "): " << r.entries.size();
            if (r.matched > r.entries.size())
                s << " из " << r.matched << " записей";
            s << "\n";
            for (const auto& e : r.entries)
                s << "  " << e.path << (e.is_dir ? "/" : "") << "\n";
            if (r.walk_capped)
                s << "[обход остановлен: осмотрено " << r.visited
                  << " записей]\n";

            ToolOutput o;
            o.title = "list " + (rel.empty() ? std::string(".") : rel);
            o.output = shell::cap(s.str(), limits::kMaxToolOutput);
            o.truncated = r.truncated || r.walk_capped;
            o.metadata = json::JsonValue::object();
            o.metadata.set("count", static_cast<long long>(r.entries.size()));
            o.metadata.set("depth", static_cast<long long>(depth));
            o.metadata.set("truncated", r.truncated);
            return o;
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
            file_lock::Guard file_guard(abs);
            if (k <= 0) return out("[ошибка] укажи k: номер строки начала (1-based)");
            std::ifstream fin(abs, std::ios::binary);
            if (!fin) return out("[ошибка] не удалось открыть файл: " + abs);
            /* И4.5: чтение через split_text, а не getline. getline оставлял
             * BOM в первой строке («\xEF\xBB\xBFa»), и при обратной
             * записи файл получал два BOM подряд. */
            const std::string raw((std::istreambuf_iterator<char>(fin)),
                                  std::istreambuf_iterator<char>());
            fin.close();
            const TextFile src = split_text(raw);
            const std::vector<std::string>& lines = src.lines;
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

            /* И4.5: формат файла (BOM/CRLF) сохраняется — иначе правка
             * двух строк превращается в «переписан весь файл». */
            TextFile tf;
            tf.lines = result;
            tf.bom = src.bom;
            tf.crlf = src.crlf;
            tf.trailing_newline = src.trailing_newline;
            const std::string new_content = join_text(tf);

            /* И11.2: обе стороны уже в руках — `raw` (прочитанное) и `new_content`
             * (то, что уйдёт в файл). */
            const diff::FileDiff fd = diff::make_file_diff(rel, raw, new_content);

            if (ctx.propose_write(rel, new_content)) {
                return with_filediff(
                    out("предложено " + rel,
                        "[предложено] " + abs + " (edit_file, строки "
                            + std::to_string(start) + "-" + std::to_string(end)
                            + ")"),
                    {fd});
            }

            std::string perm = guard_permission(ctx, abs);
            if (!perm.empty()) return out(std::move(perm));
            std::ofstream fout(abs, std::ios::binary | std::ios::trunc);
            if (!fout) return out("[ошибка] не удалось записать: " + abs);
            fout << new_content;
            fout.close();
            return with_filediff(
                out("правка " + rel,
                    "[edit_file] " + abs + ": заменены строки "
                        + std::to_string(start) + "-" + std::to_string(end)),
                {fd});
        };
        reg.register_def(std::move(def));
    }

    /* И10.3 `revert`: возврат проекта к состоянию снимка. Заменяет
     * `undo_edit` (3.5), у которого был ОДИН слот .orig на файл: вторая
     * правка того же файла затирала первую, а откат знал только про
     * последнее состояние одного файла. Снимок называет состояние всего
     * каталога, и к нему можно вернуться целиком (границы отката — в
     * шапке core/snapshot.h).
     *
     * Ключ разрешения — свой, а не `write`: откат перезаписывает проект
     * целиком, и «всегда разрешить» на его вопросе не должен молча
     * разрешать обычную запись файлов (и наоборот). Свой ключ виден
     * человеку в правилах и закрывает инструмент в профиле strict, где
     * остаётся только чтение, — обе проверки тотальны и упали бы сами.
     *
     * TF_DESTRUCTIVE — по существу, а не по привычке: содержимое файлов,
     * которое человек правил руками или уже закоммитил, откат затирает, и
     * вернуть его можно только новым откатом вперёд (10.4). Поэтому в
     * режиме плана инструмент запрещён (propose_write тут не годится:
     * предложить сотню файлов — не предложение), а в research не
     * проходит и подавно. Вопрос пользователя задаёт ToolRunner::run,
     * Enforcement-единственная-точка (правило 7). */
    {
        ToolDef def;
        def.name = "revert";
        def.description =
            "Вернуть файлы проекта к состоянию снимка. hash — tree-hash "
            "снимка (проект под git) или идентификатор копии каталога; он "
            "назван в истории сессии, в блоке «изменённые файлы», и "
            "пользователь может назвать его тебе. "
            "Пустой hash — снимок начала текущего шага, то есть состояние "
            "до правок этого хода. Файлы, которых в снимке нет (созданные "
            "после него), НЕ удаляются: git не берёт неотслеживаемые "
            "файлы в снимок, поэтому откат их не трогает.";
        def.flags = TF_WRITES_FILES | TF_EXECUTES | TF_DESTRUCTIVE | TF_SLOW;
        def.permission_key = "revert";
        SchemaBuilder b;
        b.str("hash",
              "tree-hash или идентификатор копии снимка; пусто — снимок "
              "начала текущего шага");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            const std::string hash = arg_str(a, "hash");
            const std::string project_dir = ctx.project_dir();
            const HostCallbacks& cbs = ctx.callbacks();
            const std::string data_dir =
                cbs.path_data_dir ? cbs.path_data_dir() : "";
            /* Снимок начала шага читается под локом и копируется: держать
             * лок на откате нельзя (git и копирование файлов — внутри,
             * а UI ждёт тот же мьютекс, это висящий GUI из D1). */
            snapshot::Snapshot last;
            {
                std::lock_guard<std::mutex> lk(ctx.state().mtx);
                last = ctx.state().last_snapshot;
            }
            const snapshot::RestoreResult r = snapshot::restore(
                hash, last, project_dir, snapshot::store_dir(data_dir));
            const std::string shown = trim(hash).empty()
                                          ? (last.ok() ? last.hash : std::string())
                                          : trim(hash);
            if (!r.ok) {
                return out("откат не состоялся",
                           "[revert] откат не состоялся: " + r.reason);
            }
            if (r.restored.empty() && r.leftover.empty()) {
                return out("откат нечего делать",
                           "[revert] проект уже совпадает со снимком"
                           + (shown.empty() ? std::string()
                                            : (" " + shown)) +
                           " (" + r.source + "): различающихся файлов нет");
            }
            std::string text = "[revert] состояние снимка" +
                               (shown.empty() ? std::string() : (" " + shown)) +
                               " (" + r.source + ") возвращено файлов: " +
                               std::to_string(r.restored.size()) + ".";
            /* Список ограничен, а не «сколько влезет»: откат на большом
             * проекте вернёт сотни файлов, и весь список в RESULT съел бы
             * контекст (предел общего усечения — 2000 строк, но список
             * изменений читают глазами, а не читают целиком). */
            text += file_list_lines(r.restored);
            if (!r.leftover.empty()) {
                /* Механизм НЕ угадывается: для копии каталога это отказ
                 * записи (нет прав на файл), для git-пути такого быть не
                 * должно — и выдуманная причина в ответе была бы
                 * враньём, которое модель перескажет человеку. */
                text += "Не удалось вернуть (" +
                        std::to_string(r.leftover.size()) +
                        "): после отката эти файлы всё ещё отличаются от "
                        "снимка.\n" + file_list_lines(r.leftover);
            }
            text +=
                "Файлы, которых нет в снимке, не удалены: снимок не видит "
                "неотслеживаемые файлы, и откат их не трогает.";
            return out("откат к снимку", std::move(text));
        };
        reg.register_def(std::move(def));
    }

    /* И10.4 `undo`/`redo`: переходы по стеку уровней сессии. Объявлены
     * здесь, рядом с revert, потому что это один класс действия и один
     * ключ разрешения; различает их только направление (см.
     * make_history_move_tool). */
    reg.register_def(make_history_move_tool("undo", true));
    reg.register_def(make_history_move_tool("redo", false));

    /* И8.7 `task`: делегирование задачи субагенту. Объявлен здесь, а не
     * в своём вызове регистрации, потому что он часть базового набора —
     * так его увидят и плагин, и любой тест, зовущий register_base_tools
     * (в том числе проверки агентов, которым RAG не нужен). Сам
     * инструмент живёт в core/subagent.{h,cpp}: рядом с ним — его путь
     * в область выполнения и вложенный ход, и держать объявление в
     * файле на 1600 строк значило бы прятать решение о том, кто
     * выполняет ход, в списке инструментов. */
    register_task_tool();
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

/* И9.6: описание инструмента skill_detail — краткий уровень каталога
 * навыков.
 *
 * Живёт ВНЕ анонимного пространства файла, в отличие от соседних
 * base_* помощников: объявлено в base_tools.h, потому что проверка обязана
 * доставать и до пустой ветки. SkillsManager — синглтон, копящий навыки
 * между проверками, и «навыков нет» в нём не наступает никогда, то есть
 * проверка пустого каталога через него прошла бы по причине, обратной
 * своей. В анонимном пространстве функция получила бы внутреннюю связность
 * и объявление в заголовке стало бы вторым, невидимым для проверки. */
std::string skill_detail_description(const std::vector<Skill>& skills) {
    std::string out =
        "Полный текст навыка по имени (QUERY). Доступные навыки: ";
    if (skills.empty()) {
        out += "пока пусто — вызови list_skills, чтобы проверить.";
        return out;
    }
    for (size_t i = 0; i < skills.size(); ++i) {
        if (i) out += ", ";
        out += skills[i].name;
    }
    return out;
}

} // namespace coder
