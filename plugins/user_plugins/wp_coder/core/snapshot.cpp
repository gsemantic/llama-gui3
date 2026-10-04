/*
 * snapshot.cpp — снимок состояния рабочего каталога (И10.1) и возврат к
 * нему (И10.3).
 *
 * Обоснование выбора команд и границы честности — в шапке snapshot.h;
 * здесь только код. Три места, где легко ошибиться, и все три закрыты
 * комментарием при коде:
 *
 *   - `git stash create` при чистом дереве отдаёт ПУСТОЙ вывод, а при
 *     конфликте — текст ошибки непустым. Поэтому «непустой вывод» не
 *     может быть признаком успеха: успех — это нулевой код возврата И
 *     хеш в ответе.
 *   - `git stash create` не берёт неотслеживаемые файлы (см. шапку), и
 *     `git write-tree` молча отдаёт индекс, то есть тоже не видит новых
 *     файлов. Это свойство git; обманывать его нечем.
 *   - `git restore --worktree` НЕ трогает индекс и НЕ удаляет файлы,
 *     которых нет в источнике (проверено на живом git 2.39: untracked,
 *     staged-added и созданный после снимка файл после отката остались на
 *     месте; staged — остался staged). Это ровно то поведение, которое
 *     нужно откату, — но именно поэтому список «что вернулось» нельзя
 *     доверять git: он молчит, а вернуться может не всё (каталог без
 *     прав на запись обрывает команду целиком).
 */

#include "core/snapshot.h"

#include "core/limits.h"
#include "core/shell.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <system_error>

namespace coder {
namespace snapshot {

/* Определение константы из шапки — здесь, а не в анонимном пространстве:
 * внутри анонимного пространства имя оказывается двусмысленным (оно видно
 * и как `snapshot::kOwnerSuffix`, и как безымянное), и файл перестаёт
 * собираться. */
const char* const kOwnerSuffix = ".owner";

namespace fs = std::filesystem;

namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/* Первая непустая строка: git отвечает одним значением, а на ошибке
 * пишет в stderr несколько строк (у нас они ещё и локализованы).
 * Берть весь вывод нельзя — тогда текст ошибки стал бы tree-hash. */
std::string first_line(const std::string& s) {
    size_t pos = 0;
    while (pos < s.size()) {
        size_t nl = s.find('\n', pos);
        std::string line = (nl == std::string::npos) ? s.substr(pos)
                                                      : s.substr(pos, nl - pos);
        const std::string t = trim(line);
        if (!t.empty()) return t;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return "";
}

/* Идентификатор копии каталога. Время нужно потому, что каталоги копий
 * переживают перезапуск плагина: счётчик без него дал бы второму
 * запуску то же имя и откат ушёл бы в чужую копию. */
std::string make_copy_id() {
    static std::atomic<unsigned> counter{0};
    char buf[64];
    std::snprintf(buf, sizeof(buf), "snap-%lld-%u",
                  static_cast<long long>(std::time(nullptr)),
                  counter.fetch_add(1));
    return buf;
}

/* tree-hash — это hex от 40 до 64 символов. Проверка формы обязательна:
 * без неё в Snapshot::hash мог бы попасть текст ошибки git, и 10.3
 * подставила бы его в команду восстановления. */
static bool is_hex_hash(const std::string& s) {
    if (s.size() < 40 || s.size() > 64) return false;
    for (char c : s) {
        const bool digit = (c >= '0' && c <= '9');
        const bool lower = (c >= 'a' && c <= 'f');
        if (!digit && !lower) return false;
    }
    return true;
}

/* Идентификатор копии — ровно то, что печатает make_copy_id. Форма
 * проверяется перед тем, как из неё собирается путь: хеш приходит от
 * модели, а «snap-../../..» в пути — это чтение и запись мимо каталога
 * копий. Обратная проверка (будущий формат) здесь не нужна: неизвестное
 * имя отвергается, а не угадывается. */
static bool is_copy_id(const std::string& s) {
    static const char* kPrefix = "snap-";
    const size_t prefix_len = std::strlen(kPrefix);
    if (s.size() <= prefix_len + 3) return false;
    if (s.compare(0, prefix_len, kPrefix) != 0) return false;
    size_t i = prefix_len;
    const size_t dash = s.find('-', i);
    if (dash == std::string::npos || dash == i) return false;
    for (size_t k = i; k < dash; ++k) {
        if (s[k] < '0' || s[k] > '9') return false;
    }
    for (size_t k = dash + 1; k < s.size(); ++k) {
        if (s[k] < '0' || s[k] > '9') return false;
    }
    return true;
}

} // namespace

std::string parse_hash(const std::string& output) {
    size_t pos = 0;
    while (pos < output.size()) {
        size_t nl = output.find('\n', pos);
        const std::string line = trim((nl == std::string::npos)
                                          ? output.substr(pos)
                                          : output.substr(pos, nl - pos));
        if (is_hex_hash(line)) return line;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return "";
}

namespace {

/* git с -C: тот же приём, что в core/git_tools.cpp, — timeout(1)
 * выполняет команду через execvp и не понимает встроенные команды
 * shell, поэтому `cd dir && git …` не годится. */
std::string git_cmd(const std::string& dir, const std::string& args,
                    bool& ok) {
    ok = false;
    if (dir.empty()) return "";
    std::string out;
    int code = 0;
    ok = shell::run_capture_status(
        "git -C " + shell::shell_quote(dir) + " " + args, out, code,
        limits::kSnapshotTimeoutSec);
    return out;
}

/* Копия каталога.
 *
 * Правило 9 SESSION_START (формат файла только через core/text_edit.h)
 * здесь не нарушается и не обходится: снимок копирует файлы побайтно и
 * не разбирает их содержимое — BOM и CRLF переживают копирование
 * без участия text_edit, и трогать их здесь нечем.
 *
 * Симлинки пропускаются: копирование цели разорвало бы ссылку, а
 * ссылка на каталог дало бы бесконечный обход. Пропуск — не обход
 * правил, а отказ от структуры, которой копия всё равно не может быть
 * верной.
 *
 * skip_rel — путь каталога копий относительно корня проекта.
 * Исключение обязательное, а не украшение: когда каталог данных
 * плагина оказывается ВНУТРИ проекта (так устроена фикстура тестов
 * цикла), копия без этого правила содержала бы предыдущие копии и
 * каждая следующая была бы вдвое больше предыдущей. */
bool is_under(const std::string& rel, const std::string& base) {
    if (base.empty()) return false;
    if (rel == base) return true;
    return rel.size() > base.size() &&
           rel.compare(0, base.size(), base) == 0 &&
           rel[base.size()] == '/';
}

/* Каталог копий лежит по пути `wp_coder/snapshots`, и его ПРЕДОК `wp_coder`
 * — обычная папка проекта. Обход спустится в неё раньше, чем дойдёт до
 * исключаемого каталога, поэтому каталог-предок нельзя создавать в копии:
 * иначе в копии появлялся бы пустой `wp_coder/`, которого в проекте после
 * исключения нет. Обход при этом не прекращается — в `wp_coder` могут быть
 * и обычные файлы проекта. */
bool is_ancestor_of(const std::string& rel, const std::string& base) {
    if (rel.empty() || base.empty()) return false;
    return base.size() > rel.size() &&
           base.compare(0, rel.size(), rel) == 0 &&
           base[rel.size()] == '/';
}

bool copy_tree(const fs::path& from, const fs::path& to,
               const std::string& skip_rel, std::string& err) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) {
        err = "не создать каталог копии: " + to.string() + ": " + ec.message();
        return false;
    }
    fs::recursive_directory_iterator it(
        from, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        err = "не прочитать каталог " + from.string() + ": " + ec.message();
        return false;
    }
    /* Один итератор и один конец: directory_iterator в условии цикла
     * затирает ту же error_code, которой обход и заканчивается. */
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            err = "обход " + from.string() + " прерван: " + ec.message();
            return false;
        }
        const fs::directory_entry entry = *it;
        const fs::path rel = entry.path().lexically_relative(from);
        if (rel.empty()) continue;
        const std::string rel_str = rel.string();
        if (is_under(rel_str, skip_rel)) {
            if (entry.is_directory()) it.disable_recursion_pending();
            continue;
        }
        if (entry.is_symlink()) {
            it.disable_recursion_pending();
            continue;
        }
        if (entry.is_directory()) {
            if (is_ancestor_of(rel_str, skip_rel)) continue;
            fs::create_directories(to / rel, ec);
            if (ec) {
                err = "не создать " + (to / rel).string() + ": " + ec.message();
                return false;
            }
            continue;
        }
        if (!entry.is_regular_file()) continue;
        fs::copy_file(entry.path(), to / rel,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            err = "не скопировать " + entry.path().string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

Snapshot fail(std::string reason) {
    Snapshot s;
    s.kind = Kind::None;
    s.reason = trim(reason);
    return s;
}

/* Равны ли два файла побайтно. Размер сравнивается первым: на каталоге
 * проекта это отсекает почти всё, не читая содержимое. */
bool same_content(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    const auto sa = fs::file_size(a, ec);
    if (ec) return false;
    const auto sb = fs::file_size(b, ec);
    if (ec) return false;
    if (sa != sb) return false;
    std::ifstream fa(a, std::ios::binary);
    std::ifstream fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    char ba[65536];
    char bb[65536];
    for (;;) {
        fa.read(ba, sizeof(ba));
        fb.read(bb, sizeof(bb));
        const std::streamsize na = fa.gcount();
        const std::streamsize nb = fb.gcount();
        if (na != nb) return false;
        if (na <= 0) return true;
        if (std::memcmp(ba, bb, static_cast<size_t>(na)) != 0) return false;
    }
}

/* Обычные файлы каталога относительно корня. Симлинки пропускаются по
 * той же причине, что и при копировании. Каталог копий исключать не
 * нужно: сравниваются две КОПИИ, и каждая из них уже сделана без него
 * (take() исключает), то есть лишние каталоги не возникли бы. */
void list_files(const fs::path& root, std::set<std::string>& out) {
    std::error_code ec;
    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, ec);
    if (ec) return;
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) return;
        const fs::directory_entry entry = *it;
        if (entry.is_symlink()) {
            it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file()) continue;
        const std::string rel = entry.path().lexically_relative(root).string();
        if (rel.empty()) continue;
        out.insert(rel);
    }
}

/* Содержимое файла целиком. Отказ — пустая строка, и это различается
 * с пустым файлом по вызывающему: пустой файл читается успешно и
 * тоже даёт пустую строку, поэтому неудача видна по `ok`. */
std::string read_bytes(const fs::path& path, bool& ok) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) {
        ok = false;
        return "";
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        ok = false;
        return "";
    }
    ok = true;
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

/* Отрезать от блока вывода git преамбулу (`diff --git`, `index`,
 * `new file mode`, …) и оставить unified-тело с `--- `.
 *
 * Зачем: окно рисует строки патча, а преамбула — служебная часть git.
 * Наш собственный генератор (core/diff.h) печатает ровно то же тело, и
 * без этого шага один и тот же файл выглядел бы по-разному в зависимости
 * от того, git-уровень это или копия каталога. */
std::string strip_git_preamble(const std::string& block) {
    const size_t start = block.find("--- ");
    if (start == std::string::npos) return "";
    std::string body = block.substr(start);
    /* Завершающий перевод строки убирается: наш собственный генератор
     * (core/diff.h) тоже его не оставляет, иначе окно показало бы пустую
     * строку в конце патча ИМЕННО для git-уровней, а копий каталога — нет.
     * Один и тот же файл обязан выглядеть одинаково в обоих случаях. */
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) {
        body.pop_back();
    }
    return body;
}

} // namespace

const char* Snapshot::kind_name() const {
    switch (kind) {
        case Kind::GitTree: return "git";
        case Kind::DirCopy: return "копия каталога";
        case Kind::None:    return "нет";
    }
    return "нет";
}

std::string store_dir(const std::string& data_dir) {
    if (data_dir.empty()) return "";
    return data_dir + "/wp_coder/snapshots";
}

bool is_git_repo(const std::string& project_dir) {
    bool ok = false;
    const std::string out = git_cmd(project_dir, "rev-parse --git-dir", ok);
    return ok && !trim(out).empty();
}

Snapshot take(const std::string& project_dir, const std::string& store) {
    if (project_dir.empty()) {
        return fail("не задан каталог проекта: снять нечего");
    }
    std::error_code ec;
    if (!fs::is_directory(project_dir, ec)) {
        return fail("каталог проекта не найден: " + project_dir);
    }

    if (is_git_repo(project_dir)) {
        Snapshot s;
        s.kind = Kind::GitTree;
        s.project_dir = project_dir;

        bool ok = false;
        const std::string stash_out =
            git_cmd(project_dir, "stash create", ok);
        const std::string commit = ok ? parse_hash(stash_out) : std::string();
        if (!commit.empty()) {
            /* Дерево берём из созданного коммита, а не из индекса:
             * правки агента в рабочем дереве в индексе ещё нет, и
             * write-tree назвал бы состояние ДО них. */
            std::string tree_out;
            bool tree_ok = false;
            tree_out = git_cmd(
                project_dir,
                "rev-parse " + shell::shell_quote(commit + "^{tree}"), tree_ok);
            const std::string tree = parse_hash(tree_out);
            if (tree_ok && !tree.empty()) {
                s.hash = tree;
                return s;
            }
            return fail("git не отдал дерево снимка: " + first_line(tree_out));
        }

        /* Стушить нечего (чистое дерево) либо не вышло (нет первого
         * коммита, конфликт в индексе). Индекс в обоих случаях
         * называет текущее состояние — и он дешевле дерева из коммита. */
        std::string tree_out;
        bool tree_ok = false;
        tree_out = git_cmd(project_dir, "write-tree", tree_ok);
        const std::string tree = parse_hash(tree_out);
        if (tree_ok && !tree.empty()) {
            s.hash = tree;
            return s;
        }
        return fail("git не смог назвать состояние (" +
                    first_line(stash_out) + "; " + first_line(tree_out) + ")");
    }

    /* Не git-репозиторий — копия каталога (фолбэк из строки задачи). */
    if (store.empty()) {
        return fail("проект не под git, а хранить копию некуда: "
                    "не задан каталог данных плагина");
    }
    Snapshot s;
    s.kind = Kind::DirCopy;
    s.project_dir = project_dir;
    s.hash = make_copy_id();
    s.dir = store + "/" + s.hash;
    /* Путь каталога копий относительно проекта, если он внутри него.
     * Пустая строка означает «вне проекта», и тогда исключать нечего. */
    std::string skip_rel =
        fs::path(store).lexically_relative(fs::path(project_dir)).string();
    if (skip_rel == "." || skip_rel.compare(0, 2, "..") == 0) skip_rel.clear();
    std::string err;
    if (!copy_tree(fs::path(project_dir), fs::path(s.dir), skip_rel, err)) {
        return fail("копия каталога не удалась: " + err);
    }
    /* И10.3: отметка, чьим проектом снята копия. Пишется ПОСЛЕ копии и
     * перед возвратом: копия без отметки вернулась бы, но откат по ней
     * был бы невозможен (restore отвергает её с названной причиной), то
     * есть это был бы снимок, который только кажется откатываемым.
     *
     * Если отметку записать не удалось, копия сносится: оставить её значит
     * оставить в каталоге копий мусор, который ничем не читается. */
    const std::string owner = s.dir + kOwnerSuffix;
    {
        std::ofstream f(owner, std::ios::binary | std::ios::trunc);
        if (!f) {
            fs::remove_all(fs::path(s.dir), ec);
            return fail("копия каталога непригодна: не записать отметку "
                        + owner);
        }
        f << project_dir << "\n";
        if (!f.good()) {
            fs::remove_all(fs::path(s.dir), ec);
            fs::remove(owner, ec);
            return fail("копия каталога непригодна: отметка записана не "
                        "вся: " + owner);
        }
    }
    return s;
}

std::vector<std::string> diff_dirs(const std::string& before,
                                   const std::string& after) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(before, ec) || !fs::is_directory(after, ec)) {
        return out;
    }
    std::set<std::string> in_before;
    std::set<std::string> in_after;
    list_files(fs::path(before), in_before);
    list_files(fs::path(after), in_after);
    /* Два обхода, и порядок результата задан множествами: порядок
     * обхода каталога не определён, а список файлов попадает в файл
     * сессии и в UI — неопределённый порядок означал бы две сессии с
     * разным текстом на одном и том же шаге. */
    for (const std::string& rel : in_after) {
        const bool known = in_before.count(rel) != 0;
        if (!known ||
            !same_content(fs::path(before) / rel, fs::path(after) / rel)) {
            out.push_back(rel);
        }
    }
    for (const std::string& rel : in_before) {
        if (in_after.count(rel) == 0) out.push_back(rel);
    }
    /* Общая сортировка: два обхода дали бы «сначала из after, потом
     * удалённые», то есть два разных порядка в зависимости от того,
     * что оказалось удалением. Список попадает в файл сессии и в UI, и
     * один и тот же шаг обязан выглядеть одинаково при любом исходе.
     * Порядок — побайтовый, как у git: два пути должны читаться
     * одинаково, какой бы путь их ни назвал. */
    std::sort(out.begin(), out.end());
    return out;
}

/* Вывод git с -z разделён NUL, а не переводом строки: без -z git печатает
 * имя с пробелом или не-ASCII в кавычках с восьмеричными побегами
 * ("\320\270\320\274..."), и в файл сессии ушёл бы не путь, а его
 * текстовое представление. Проверено на живом git 2.39. */
static std::vector<std::string> split_nul(const std::string& s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t z = s.find('\0', pos);
        if (z == std::string::npos) {
            out.push_back(s.substr(pos));
            break;
        }
        if (z > pos) out.push_back(s.substr(pos, z - pos));
        pos = z + 1;
    }
    return out;
}

FileChanges changed_files(const Snapshot& before, const Snapshot& after) {
    FileChanges ch;
    if (!before.ok() || !after.ok()) {
        ch.reason = "сравнивать нечего: один из снимков не состоялся (" +
                    std::string(before.ok() ? after.reason : before.reason) + ")";
        return ch;
    }
    if (before.kind != after.kind) {
        ch.reason = std::string("снимки разных видов: ") + before.kind_name() +
                    " и " + after.kind_name();
        return ch;
    }
    if (before.kind == Kind::GitTree) {
        if (before.hash == after.hash) {
            ch.ok = true;
            return ch;
        }
        /* Сравниваются два ДЕРЕВА, а не дерево и рабочий каталог: между
         * двумя командами git состояние могло уехать, и тогда ответ был
         * бы верен только для момента, который никто не запоминал.
         *
         * --relative обязателен: git печатает пути от КОРНЯ репозитория,
         * а проект плагина — возможно подкаталог, и путь «wp-content/…»
         * из корня монорепозитория для агента не значил бы ничего. */
        std::string out;
        int code = 0;
        const std::string cmd =
            "git -C " + shell::shell_quote(before.project_dir) +
            " diff --name-only --relative -z " + shell::shell_quote(before.hash) +
            " " + shell::shell_quote(after.hash);
        shell::run_capture_status(cmd, out, code, limits::kSnapshotTimeoutSec);
        if (code != 0) {
            ch.reason = "git не сравнил снимки: " + first_line(out);
            return ch;
        }
        ch.files = split_nul(out);
        ch.ok = true;
        return ch;
    }
    ch.files = diff_dirs(before.dir, after.dir);
    ch.ok = true;
    return ch;
}

/* ======================================================================
 * И10.3: возврат к состоянию снимка
 * ====================================================================== */

namespace {

/* Файлы проекта, отличающиеся от дерева снимка (git-путь).
 *
 * --relative обязателен по той же причине, что в changed_files: путь от
 * корня репозитория агенту ничего не значит, а проект — возможно
 * подкаталог. Пустой список при нулевом коде — «совпадает», и это не
 * ошибка. */
bool diff_from_tree(const std::string& dir, const std::string& tree,
                    std::vector<std::string>& out, std::string& err) {
    std::string text;
    int code = 0;
    const std::string cmd =
        "git -C " + shell::shell_quote(dir) +
        " diff --name-only --relative -z " + shell::shell_quote(tree);
    shell::run_capture_status(cmd, text, code, limits::kSnapshotTimeoutSec);
    if (code != 0) {
        err = first_line(text);
        return false;
    }
    out = split_nul(text);
    return true;
}

/* Откат по дереву git.
 *
 * Pathspec — точка: без неё git вернул бы весь репозиторий, а проект
 * плагина может быть подкаталком чужого монорепозитория (проверено на
 * живом git 2.39: `git -C <sub> restore -- .` вернул только файлы под
 * sub, а лежащий рядом файл в корне репозитория не тронул). Точка
 * разрешается относительно каталога, который задаёт -C, — то есть
 * относительно проекта, а не относительно рабочего каталога хоста.
 *
 * --worktree без --staged: индекс неприкосновенен. Снимок — это рабочее
 * дерево, и его возврат не должен переписывать то, что человек staged
 * (проверено на живом git 2.39: `git diff --cached` до и после отката
 * совпал).
 *
 * Файловых семафоров здесь НЕТ, и это не недосмотр, а разобранная
 * невозможность. `file_lock::Guard` берёт одну из 64 нерекурсивных полос
 * по хешу пути, поэтому N Guard'ов на N путях — это дедлок на первой же
 * паре путей, попавших в одну полосу (на 60 файлах это не «возможно», а
 * фактически всегда: рождений по полосам 64 хватает с большим запасом).
 * Обойти это нечем: полосы не видны снаружи, а брать полосу на весь каталог
 * значило бы защитить от записи в каталог, тогда как пишущие инструменты
 * берут полосу по ФАЙЛУ, — то есть такая блокировка ничего бы не
 * исключала, а выглядела бы защищённой.
 *
 * Гонки с другим писателем при этом нет: второй писатель из UI —
 * `Engine::pending_apply`, а он берёт содержимое из `state.pending`,
 * которое наполняет только `ToolContext::propose_write`, а тот вне режима
 * плана возвращается сразу. Откат в режиме плана запрещён (TF_DESTRUCTIVE),
 * поэтому «человек жмёт «применить» в тот же момент» недостижимо. */
RestoreResult restore_git(const std::string& tree,
                          const std::string& project_dir) {
    RestoreResult r;
    r.source = "git";
    std::vector<std::string> before;
    std::string err;
    if (!diff_from_tree(project_dir, tree, before, err)) {
        r.reason = "git не сравнил проект со снимком: " + err;
        return r;
    }
    if (before.empty()) {
        r.ok = true;
        return r;
    }

    std::string text;
    int code = 0;
    const std::string cmd =
        "git -C " + shell::shell_quote(project_dir) +
        " restore --source=" + shell::shell_quote(tree) + " --worktree -- .";
    shell::run_capture_status(cmd, text, code, limits::kSnapshotTimeoutSec);
    if (code != 0) {
        r.reason = "git не вернул состояние снимка: " + first_line(text);
        return r;
    }

    /* Проверка ПОСЛЕ, а не по списку «до»: git молчит об успехе, и файл
     * мог остаться отличным (например, его нельзя записать). Считать
     * успехом сам факт команды значило бы сообщить модели «проект приведён
     * к снимку» там, где он не приведён. */
    std::vector<std::string> after;
    if (!diff_from_tree(project_dir, tree, after, err)) {
        r.reason = "git вернул состояние, но проверить его не удалось: " + err;
        return r;
    }
    const std::set<std::string> still_different(after.begin(), after.end());
    for (const std::string& rel : before) {
        if (still_different.count(rel) == 0) r.restored.push_back(rel);
    }
    r.leftover = after;
    r.ok = true;
    return r;
}

/* Откат по копии каталога (проект не под git).
 *
 * Отметка проекта сверяется ДО любой записи: каталог копий общий для всех
 * проектов плагина, и копия чужого проекта, возвращённая в текущий,
 * залила бы его файлами того — молча и целиком (правило 3 в шапке).
 *
 * Отсутствующие в копии файлы НЕ удаляются (правило 1 в шапке): копия
 * снята целиком, и «нет в копии» означает лишь «нечего верчать», а не
 * «удалить». */
RestoreResult restore_copy(const std::string& copy_id,
                            const std::string& project_dir,
                            const std::string& store) {
    RestoreResult r;
    r.source = "копия каталога";
    if (store.empty()) {
        r.reason = "не задан каталог данных плагина: копии снимков "
                   "недоступны, откатить копию нечем";
        return r;
    }
    std::error_code ec;
    const std::string dir = store + "/" + copy_id;
    if (!fs::is_directory(dir, ec)) {
        r.reason = "снимка нет: копия " + copy_id + " не найдена (" + dir + ")";
        return r;
    }

    const std::string owner_path = dir + kOwnerSuffix;
    std::string owner;
    {
        std::ifstream f(owner_path, std::ios::binary);
        if (f) {
            owner = std::string((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
        }
    }
    owner = trim(owner);
    if (owner.empty()) {
        r.reason = "копия снимка без отметки проекта (" + owner_path +
                   "): откат по ней невозможен";
        return r;
    }
    if (owner != project_dir) {
        r.reason = "снимок снят с другого проекта (" + owner +
                   "), а откат запрошен для " + project_dir;
        return r;
    }

    std::set<std::string> in_copy;
    list_files(fs::path(dir), in_copy);

    std::vector<std::string> pending;
    for (const std::string& rel : in_copy) {
        const fs::path from = fs::path(dir) / rel;
        const fs::path to = fs::path(project_dir) / rel;
        std::error_code exists_ec;
        if (fs::exists(to, exists_ec) && same_content(from, to)) continue;
        pending.push_back(rel);
    }
    if (pending.empty()) {
        r.ok = true;
        return r;
    }

    std::vector<std::string> failed;
    for (const std::string& rel : pending) {
        const fs::path from = fs::path(dir) / rel;
        const fs::path to = fs::path(project_dir) / rel;
        std::error_code copy_ec;
        fs::create_directories(to.parent_path(), copy_ec);
        if (!copy_ec) {
            fs::copy_file(from, to, fs::copy_options::overwrite_existing,
                          copy_ec);
        }
        /* Копия побайтная (правило 9 шапки snapshot.cpp): BOM и CRLF
         * переживают её без участия text_edit, и трогать их нечем. */
        if (copy_ec || !same_content(from, to)) failed.push_back(rel);
    }
    for (const std::string& rel : pending) {
        if (std::find(failed.begin(), failed.end(), rel) == failed.end()) {
            r.restored.push_back(rel);
        }
    }
    r.leftover = failed;
    r.ok = true;
    return r;
}

} // namespace

RestoreResult restore(const std::string& hash, const Snapshot& last,
                      const std::string& project_dir, const std::string& store) {
    RestoreResult r;
    if (project_dir.empty()) {
        r.reason = "не задан каталог проекта: откатывать нечего";
        return r;
    }
    std::error_code ec;
    if (!fs::is_directory(project_dir, ec)) {
        r.reason = "каталог проекта не найден: " + project_dir;
        return r;
    }

    /* Вид снимка — по форме хеша (см. шапку). Форма проверяется ДО
     * подстановки в команду и ДО сборки пути: хеш приходит от модели, а
     * «идентификатор копии» превращается в путь. */
    const std::string h = trim(hash);
    if (h.empty()) {
        if (!last.ok()) {
            r.reason = "снимка начала шага нет: " +
                       (last.reason.empty() ? std::string("снимок не снят")
                                            : last.reason) +
                       ". Укажи hash явно.";
            return r;
        }
        if (last.kind == Kind::GitTree) return restore_git(last.hash, project_dir);
        return restore_copy(last.hash, project_dir, store);
    }
    if (is_hex_hash(h)) {
        if (!is_git_repo(project_dir)) {
            r.reason = "hash похож на tree-hash git, а каталог " + project_dir +
                       " не под git: восстановить нечего";
            return r;
        }
        return restore_git(h, project_dir);
    }
    if (is_copy_id(h)) return restore_copy(h, project_dir, store);

    r.reason = "hash не похож ни на tree-hash git, ни на идентификатор "
               "копии каталога: " + h;
    return r;
}

/* ======================================================================
 * И10.4: стек уровней сессии (undo/redo)
 *
 * Правила перехода — в шапке snapshot.h; здесь только код. Четыре места,
 * где легко ошибиться, и все четыре закрыты комментарием при коде:
 *
 *   - Уровень — СОСТОЯНИЕ, а не шаг: снимок, совпавший с последним
 *     уровнем, новым уровнем не становится, и «отменить» не приходится
 *     жать вхолостую после шага, который ничего не менял.
 *   - Одно нажатие отменяет ОДИН шаг, а не два. Отсюда различие в цели:
 *     когда проект УПЕРЕДИ уровня (правки текущего шага), идём к самому
 *     верхнему уровню, а когда стоит на нём — к предыдущему. Без этого
 *     различения второе нажатие съедало бы два шага.
 *   - Возврат вперёд умирает на новой работе (push), а отмена назад — нет.
 *   - Стек меняется ПОСЛЕ того, как проект вернулся к уровню: иначе
 *     неудачный переход (нет копии, объект собран git, каталог без прав)
 *     стёр бы то, к чему не дошёл, то есть отказ съел бы право вернуться.
 * ====================================================================== */

namespace {

/* Одно и то же состояние?
 *
 * Уровень — это СОСТОЯНИЕ, а не его копия, и сравнивать надо именно так.
 * Дерево git сравнивается по tree-hash: дёшево и точно. А копии каталога
 * сравниваются ПО СОДЕРЖИМОМУ (diff_dirs), и это не оптимизация, а
 * исправление: идентификатор копии уникален на каждый take(), и по нему
 * сравнение всегда давало бы «состояния разные». На проекте без git это
 * означало бы две поломки сразу — каждый шаг становился бы уровнем, даже
 * когда агент ничего не менял, и отмена ходила бы по кругу, не сходясь с
 * уровня (именно это и поймал сторож тестов на проверке глубины).
 *
 * Вид тоже сравнивается: переход проекта под git (или обратно) меняет то,
 * чем снимок назван, и уровень другого вида — не этот. Отсутствующая
 * копия не равна ничему: сравнивать нечем, и «наверное, то же самое»
 * здесь было бы отказом от проверки вхолостую.
 *
 * Цена решения названа: на не-git-проекте сравнение уровней стоит обхода
 * дерева, то есть цена уровня удваивается — ровно та же цена, что и
 * копирование каталога (отклонение 106). */
bool same_state(const Snapshot& a, const Snapshot& b) {
    if (!a.ok() || !b.ok() || a.kind != b.kind) return false;
    if (a.kind == Kind::GitTree) return a.hash == b.hash;
    if (a.dir.empty() || b.dir.empty()) return false;
    std::error_code ec;
    if (!fs::is_directory(a.dir, ec) || !fs::is_directory(b.dir, ec)) {
        return false;
    }
    return diff_dirs(a.dir, b.dir).empty();
}

} // namespace

void discard_copies(const std::vector<Snapshot>& snapshots) {
    std::error_code ec;
    for (const Snapshot& s : snapshots) {
        /* Только копии каталога: у git-уровня объекты лежат в базе
         * репозитория, и удалять их отсюда нечем (и незачем — за это есть
         * git gc). */
        if (s.kind != Kind::DirCopy || s.dir.empty()) continue;
        fs::remove_all(fs::path(s.dir), ec);
        ec.clear();
        fs::remove(fs::path(s.dir + kOwnerSuffix), ec);
        ec.clear();
    }
}

/* --- И10.5: diff для показа в UI --- */

size_t DiffReport::additions() const {
    size_t n = 0;
    for (const diff::FileDiff& f : files) n += f.additions;
    return n;
}

size_t DiffReport::deletions() const {
    size_t n = 0;
    for (const diff::FileDiff& f : files) n += f.deletions;
    return n;
}

namespace {

/* Один файл по `git diff --numstat -z`: имя и СЧЁТЧИКИ строк.
 *
 * Имена берутся ОТСЮДА, а не из заголовков unified-патча, и это не
 * выбор вкуса: у git есть два вида изменений, для которых строк
 * `--- a/путь` / `+++ b/путь` НЕ ВООБЩЕ печатается — двоичный файл
 * (`Binary files … differ`) и изменение только прав (`old mode`/
 * `new mode`). В обоих случаях имя есть только в заголовке
 * `diff --git a/X b/Y`, а он неоднозначен: имя с пробелом там
 * неотделимо от следующего поля (живой git печатает
 * `diff --git a/ascii space.txt b/ascii space.txt`).
 *
 * `--numstat -z` печатает `добавлено<TAB>удалено<TAB>путь` и НЕ
 * цитирует путь (проверено на живом git 2.39), то есть имя приходит
 * однозначным. У двоичного файла счётчики — `-\t-`, и это признак
 * «строк нет», а не «нулевые строки». */
struct NumstatEntry {
    std::string file;
    size_t additions = 0;
    size_t deletions = 0;
    bool binary = false;
};

std::vector<NumstatEntry> parse_numstat_z(const std::string& out) {
    std::vector<NumstatEntry> entries;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t nul = out.find('\0', pos);
        const std::string record =
            nul == std::string::npos ? out.substr(pos) : out.substr(pos, nul - pos);
        pos = nul == std::string::npos ? out.size() : nul + 1;
        if (record.empty()) continue;
        const size_t t1 = record.find('\t');
        if (t1 == std::string::npos) continue;
        const size_t t2 = record.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        NumstatEntry e;
        e.file = record.substr(t2 + 1);
        const std::string add = record.substr(0, t1);
        const std::string del = record.substr(t1 + 1, t2 - t1 - 1);
        if (add == "-" || del == "-") {
            e.binary = true;
        } else {
            e.additions = static_cast<size_t>(std::strtoul(add.c_str(), nullptr, 10));
            e.deletions = static_cast<size_t>(std::strtoul(del.c_str(), nullptr, 10));
        }
        if (!e.file.empty()) entries.push_back(e);
    }
    return entries;
}

/* Разбор unified-вывода и склейка его с numstat.
 *
 * Порядок блоков совпадает с порядком numstat (оба сортируются путём), и
 * ЭТО ПРОВЕРЯЕТСЯ: у блока, у которого имя есть, сверяется, что оно
 * совпало с ожидаемым из numstat. Расхождение — отказ с названной
 * причиной, а не показ diff чужого файла: в блоке может быть файл с
 * двоичным содержимым без единой строки, и ошибка в сортировке выдала
 * бы его патч за патч соседа. */
bool parse_git_diff(const std::string& out,
                    const std::vector<NumstatEntry>& numstat,
                    DiffReport& report);

} // namespace

bool diff_from_git_text(const std::string& numstat_z, const std::string& patch,
                        DiffReport* out) {
    DiffReport report;
    const std::vector<NumstatEntry> numstat = parse_numstat_z(numstat_z);
    if (!parse_git_diff(patch, numstat, report)) {
        *out = report;
        return false;
    }
    if (report.files.size() + report.omitted < numstat.size()) {
        /* Файлов в numstat больше, чем блоков в выводе патча: обычно это
         * изменение только прав (git печатает для него блок, но строк
         * нет), и тогда отчёт неполон без предупреждения. */
        report.truncated = true;
        report.omitted += numstat.size() - (report.files.size() + report.omitted);
    }
    report.ok = true;
    *out = report;
    return true;
}

namespace {

bool parse_git_diff(const std::string& out,
                    const std::vector<NumstatEntry>& numstat,
                    DiffReport& report) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < out.size()) {
        const size_t nl = out.find('\n', pos);
        if (nl == std::string::npos) {
            lines.push_back(out.substr(pos));
            break;
        }
        lines.push_back(out.substr(pos, nl - pos));
        pos = nl + 1;
    }

    const char kBlockHead[] = "diff --git ";
    const size_t kBlockHeadLen = sizeof(kBlockHead) - 1;
    size_t i = 0;
    size_t index = 0;
    while (i < lines.size()) {
        if (lines[i].compare(0, kBlockHeadLen, kBlockHead) != 0) { ++i; continue; }
        size_t j = i + 1;
        std::string block = lines[i] + "\n";
        bool has_binary_line = false;
        bool has_mode_only = false;
        while (j < lines.size() &&
               lines[j].compare(0, kBlockHeadLen, kBlockHead) != 0) {
            if (lines[j].compare(0, 13, "Binary files ") == 0) has_binary_line = true;
            /* Длина 9, а не 8: «old mode » — девять символов вместе с
             * пробелом, и с восемью сравнение никогда не сходилось, то
             * есть изменение прав проходило как файл без строк и без
             * объяснения. Поймано проверкой на тексте вывода git. */
            if (lines[j].compare(0, 9, "old mode ") == 0) has_mode_only = true;
            block += lines[j] + "\n";
            ++j;
        }
        i = j;

        /* Имя из заголовков — там, где они есть. */
        std::string minus_field, plus_field;
        {
            size_t p2 = 0;
            bool in_hunk = false;
            while (p2 < block.size()) {
                const size_t nl = block.find('\n', p2);
                const std::string line =
                    nl == std::string::npos ? block.substr(p2)
                                            : block.substr(p2, nl - p2);
                if (!in_hunk) {
                    if (line.compare(0, 4, "--- ") == 0) {
                        minus_field = line.substr(4);
                    } else if (line.compare(0, 4, "+++ ") == 0) {
                        plus_field = line.substr(4);
                    }
                    if (line.compare(0, 3, "@@ ") == 0) in_hunk = true;
                }
                if (nl == std::string::npos) break;
                p2 = nl + 1;
            }
        }
        std::string name = diff::parse_git_header_name(minus_field);
        if (name.empty()) name = diff::parse_git_header_name(plus_field);

        if (index >= numstat.size()) {
            report.ok = false;
            report.reason =
                "git напечатал больше блоков изменений, чем файлов в "
                "numstat: показывать diff не того файла нельзя";
            return false;
        }
        const NumstatEntry& e = numstat[index];
        ++index;
        if (!name.empty() && name != e.file) {
            report.ok = false;
            report.reason = "git напечатал блок для «" + name +
                            "», а в numstat на этом месте «" + e.file +
                            "»: показывать diff не того файла нельзя";
            return false;
        }

        diff::FileDiff fd;
        fd.file = e.file;
        fd.binary = e.binary;
        fd.additions = e.additions;
        fd.deletions = e.deletions;
        if (e.binary) {
            fd.patch = "";
            fd.note = "двоичный файл: показать нечем, строк нет";
        } else if (has_mode_only && has_binary_line == false && e.additions == 0 &&
                   e.deletions == 0) {
            /* Права изменились, содержимое нет. Строк нет — и сказать
             * «правок нет» было бы неправдой: изменение есть, просто
             * не в строках. */
            fd.patch = "";
            fd.note = "изменились только права файла, строк нет";
        } else {
            fd.patch = strip_git_preamble(block);
        }
        if (report.files.size() < limits::kMaxDiffFiles) {
            report.files.push_back(fd);
        } else {
            report.truncated = true;
            ++report.omitted;
        }
    }
    return true;
}

/* Копия каталога: файлы, которых нет с одной стороны или которые
 * разошлись побайтно. Симлинки пропускаются — тем же правилом, что при
 * копировании (diff_dirs). */
void diff_copy_dirs(const Snapshot& before, const Snapshot& after,
                    DiffReport& report) {
    std::set<std::string> in_before, in_after;
    list_files(fs::path(before.dir), in_before);
    list_files(fs::path(after.dir), in_after);
    std::set<std::string> all = in_before;
    all.insert(in_after.begin(), in_after.end());
    for (const std::string& rel : all) {
        if (in_before.count(rel) && in_after.count(rel) &&
            same_content(fs::path(before.dir) / rel, fs::path(after.dir) / rel)) {
            continue;
        }
        if (report.files.size() >= limits::kMaxDiffFiles) {
            report.truncated = true;
            ++report.omitted;
            continue;
        }
        bool ok_before = false, ok_after = false;
        const std::string content_before =
            read_bytes(fs::path(before.dir) / rel, ok_before);
        const std::string content_after =
            read_bytes(fs::path(after.dir) / rel, ok_after);
        if (!ok_before || !ok_after) {
            /* Файл не прочитался — показать его нечем, и молча пропустить
             * значило бы «правок нет» при файле, который изменился. */
            diff::FileDiff fd;
            fd.file = rel;
            fd.note = ok_before ? "файл после состояния не читается"
                                : "файл до состояния не читается";
            report.files.push_back(fd);
            continue;
        }
        report.files.push_back(
            diff::make_file_diff(rel, content_before, content_after));
    }
}

} // namespace

DiffReport diff_states(const Snapshot& before, const Snapshot& after,
                       const std::string& store_dir) {
    (void)store_dir;
    DiffReport report;
    report.from_hash = before.hash;
    report.to_hash = after.hash;
    if (!before.ok() || !after.ok()) {
        report.reason = "сравнивать нечем: снимок не состоялся";
        if (!before.ok()) report.reason += " (до: " + before.reason + ")";
        if (!after.ok()) report.reason += " (после: " + after.reason + ")";
        return report;
    }
    if (before.kind != after.kind) {
        /* Снимки разных видов сравнить нечем: git-путь работает с двумя
         * деревьями, копия — с двумя каталогами. Молча взять «что-то
         * одно» значило бы показать diff не того. */
        report.reason = std::string("снимки разных видов: до — ") +
                        before.kind_name() + ", после — " + after.kind_name();
        return report;
    }

    if (before.kind == Kind::GitTree) {
        std::error_code ec;
        if (!fs::is_directory(before.project_dir, ec)) {
            report.reason = "каталог проекта не найден: " + before.project_dir;
            return report;
        }
        const std::string range = shell::shell_quote(before.hash) + " " +
                                  shell::shell_quote(after.hash);
        /* Два вызова, и оба нужны. Первый даёт ИМЕНА и СЧЁТЧИКИ однозначно
         * (numstat -z не цитирует путь), второй — тела патчей. Одним
         * вызовом обойтись нельзя: у двоичного файла и у изменения только
         * прав в unified-выводе нет ни `---`, ни `+++`, то есть имя там
         * существует только в неоднозначном заголовке `diff --git`. */
        bool stat_ok = false;
        const std::string stat_out = git_cmd(
            before.project_dir,
            "diff --numstat -z --no-renames --relative " + range + " -- .",
            stat_ok);
        if (!stat_ok) {
            report.reason = "git не отдал список изменённых файлов: " +
                            first_line(stat_out);
            return report;
        }
        bool cmd_ok = false;
        const std::string out = git_cmd(
            before.project_dir,
            "diff --no-color --no-renames -U3 --relative " + range + " -- .",
            cmd_ok);
        if (!cmd_ok) {
            report.reason = "git не отдал diff: " + first_line(out);
            return report;
        }
        /* Разбор — в diff_from_git_text(), отдельной функцией: сверку
         * вывода git с живым репозиторием нельзя заставить
         * детерминированно (отклонение 110 — там же про parse_hash).
         * Шов заполняет отчёт целиком, поэтому хеши, о которых знает
         * только вызывающий, проставляются ПОСЛЕ — иначе отчёт потерял
         * бы «из какого состояния в какое», а это ровно то, что человек
         * читает в заголовке. */
        if (!diff_from_git_text(stat_out, out, &report)) {
            report.from_hash = before.hash;
            report.to_hash = after.hash;
            return report;
        }
        report.from_hash = before.hash;
        report.to_hash = after.hash;
    } else {
        diff_copy_dirs(before, after, report);
    }
    report.ok = true;
    return report;
}

DiffReport UndoStack::diff_step(size_t index, const std::string& project_dir,
                                const std::string& store_dir) const {
    DiffReport report;
    if (levels_.empty()) {
        report.reason = "ступеней нет: агент ещё не делал шагов";
        return report;
    }
    if (index >= levels_.size()) {
        report.reason = "уровня " + std::to_string(index) + " нет: стек содержит " +
                        std::to_string(levels_.size());
        return report;
    }
    const Snapshot& from = levels_[index];
    if (index + 1 < levels_.size()) {
        return diff_states(from, levels_[index + 1], store_dir);
    }
    /* Последний уровень: «куда» — это то, что на диске сейчас. Снимок
     * берётся здесь, и на не-git-проекте это ПОЛНАЯ копия каталога —
     * цена названа в шапке diff_step(), потому что её платит тот, кто
     * спросил (окно), а не агент. */
    const Snapshot here = take(project_dir, store_dir);
    const DiffReport computed = diff_states(from, here, store_dir);
    /* Снимок «куда» не принадлежит стеку и не переживает вызов, поэтому
     * убирается ЗДЕСЬ: правило «копии удаляет вызывающий, потому что лок
     * кончается» относится к копиям стека, а этот снимок снят и прочитан
     * в одном вызове, вне лока. Оставить его было бы утечкой на каждом
     * нажатии кнопки — то есть на каждом показе diff верхнего уровня. */
    discard_copies(std::vector<Snapshot>{here});
    return computed;
}

bool UndoStack::push(const Snapshot& s) {
    /* Снимок не состоялся — уровня нет. Причина остаётся в самом снимке:
     * терять её нельзя, отказ потом скажет именно ею. */
    if (!s.ok()) return false;

    /* Смена проекта обнуляет стек. Сверка идёт по полю снимка, а не по
     * отдельному флагу «проект сменился»: проект задаётся настройкой и
     * меняется из UI прямой записью в состояние, крючка на смену там
     * нет, а уровни чужого проекта вернули бы сюда его файлы (у похожих
     * проектов имена файлов совпадают). */
    if (!project_dir_.empty() && project_dir_ != s.project_dir) {
        trash_.insert(trash_.end(), levels_.begin(), levels_.end());
        levels_.clear();
        pos_ = 0;
    }
    project_dir_ = s.project_dir;

    /* Новая работа убивает возврат вперёд: уровни впереди больше не
     * достижимы. Отсчёт от pos_, а не от конца — после отмены вперёди
     * лежит хвост, а не один последний уровень. */
    if (pos_ + 1 < levels_.size()) {
        trash_.insert(trash_.end(), levels_.begin() + static_cast<long>(pos_) + 1,
                      levels_.end());
        levels_.erase(levels_.begin() + static_cast<long>(pos_) + 1,
                      levels_.end());
    }
    /* Тот же уровень дважды не заводим: шаг, ничего не изменивший, не
     * должен стоить нажатия «отменить». */
    if (!levels_.empty() && same_state(s, levels_.back())) {
        return false;
    }
    levels_.push_back(s);
    pos_ = levels_.size() - 1;

    /* Глубина ограничена: уровень на проекте без git — полная копия
     * каталога, и без предела каталог данных плагина рос бы всю сессию.
     * Вытесненный уровень отменить уже нельзя, и его копия уходит в
     * мусор, который удалит вызывающий.
     *
     * Здесь условия «не вытеснять текущий уровень» НЕТ и быть не может:
     * позиция только что поставлена в levels_.size() - 1, а цикл крутится
     * только когда размер больше предела, то есть pos_ тут заведомо больше
     * нуля. Мёртвое условие было написано «на всякий случай» и обмануло
     * мутационный прогон: снятие его ничего не меняло, и прогон объявил бы
     * выжившей мутацию, которая на деле ничего не трогала. Настоящая
     * защита стоит в move() (там позиция может быть нулевой). */
    while (levels_.size() > limits::kSnapshotStackDepth) {
        trash_.push_back(levels_.front());
        levels_.erase(levels_.begin());
        --pos_;
    }
    return true;
}

void UndoStack::clear() {
    trash_.insert(trash_.end(), levels_.begin(), levels_.end());
    levels_.clear();
    pos_ = 0;
    project_dir_.clear();
}

std::vector<Snapshot> UndoStack::take_trash() {
    std::vector<Snapshot> out;
    out.swap(trash_);
    return out;
}

void UndoStack::report_position(MoveResult& r) const {
    /* Нулевой уровень при пустом стеке — не «первый уровень», а «считать
     * нечего»: номер без количества читался бы как позиция. */
    r.level = levels_.empty() ? 0 : pos_ + 1;
    r.levels = levels_.size();
    r.can_undo = can_undo();
    r.can_redo = can_redo();
}

bool UndoStack::refuse_foreign(MoveResult& r,
                              const std::string& project_dir) const {
    if (project_dir.empty()) {
        r.reason = "не задан каталог проекта: переходить не по чему";
        return true;
    }
    if (!levels_.empty() && !project_dir_.empty() &&
        project_dir != project_dir_) {
        r.reason = "уровни стека сняты с другого проекта (" + project_dir_ +
                   "), а переход запрошен для " + project_dir;
        return true;
    }
    return false;
}

MoveResult UndoStack::move(int dir, const Snapshot& target,
                           const Snapshot& here, const std::string& project_dir,
                           const std::string& store_dir) {
    MoveResult r;
    if (refuse_foreign(r, project_dir)) {
        report_position(r);
        return r;
    }
    if (!target.ok()) {
        r.reason = "уровня нет: " + target.reason;
        report_position(r);
        return r;
    }

    const RestoreResult res = restore(target.hash, target, project_dir, store_dir);
    if (!res.ok) {
        /* Снятое ради перехода состояние в стек не попало и уже не
         * пригодится: на проекте без git это копия целого каталога, и её
         * убираем сразу, иначе она осталась бы на диске навсегда.
         *
         * Совпадение состояния с уровнем НЕ делает копию чужой: у снимка
         * DirCopy каталог свой (идентификатор выдаёт take()), и стек на
         * этот каталог не ссылается — в него попадает только то, что
         * кладёт push(). Раньше здесь стояло `!holds(here)`, то есть
         * «уровень с таким состоянием есть — копию не выбрасывать», и
         * мутация, снимавшая это условие, ВЫЖИЛА: утечку никто не
         * наблюдал. */
        trash_.push_back(here);
        r.reason = res.reason;
        report_position(r);
        return r;
    }

    /* Позиция — это индекс ЦЕЛИ в стеке, и ни вперёд, ни назад стек не
     * пересобирается целиком.
     *
     * ВПЕРЁД цель уже лежит в стеке (это levels_[pos_+1]), поэтому
     * переход двигает ТОЛЬКО pos_. Уровни впереди — это цели возврата,
     * и стирать их здесь нельзя: первая версия пересобирала стек по
     * префиксу и вместе с целями возврата отбрасывала всё, что впереди, —
     * после чего второй возврат вперёд был невозможен, а стек с каждым
     * шагом назад терял уровни.
     *
     * НАЗАД цель — это либо levels_[pos_] (проект упирался вперёд, то
     * есть отменяются правки текущего шага), либо levels_[pos_-1]. В
     * первом случае уходящее состояние встаёт НАД отменённым — иначе
     * вернуться вперёд было бы некуда. Считать индекс цели в новом
     * списке пришлось после первой поломки: pos_ = pos_ - 1 «на всякий
     * случай» уводил позицию на уровень ниже цели (второе нажатие
     * отменяло шаг через один), а на третьем pos_ уходил под ноль и
     * индекс читался мимо массива. */
    const bool ahead = !same_state(here, levels_[pos_]);
    if (dir > 0) {
        pos_ += 1;
    } else if (ahead) {
        levels_.push_back(here);
    } else {
        --pos_;
    }

    /* Глубина: единственное место, где переход может её превысить —
     * отмена с новым уровнем над отменённым. Условие pos_ > 0 — то же, что
     * в push(): уровень, на котором стоит проект, не вытесняется. */
    while (levels_.size() > limits::kSnapshotStackDepth && pos_ > 0) {
        trash_.push_back(levels_.front());
        levels_.erase(levels_.begin());
        --pos_;
    }

    r.ok = true;
    r.source = res.source;
    r.level_hash = target.hash;
    r.restored = res.restored;
    r.leftover = res.leftover;
    report_position(r);
    return r;
}

MoveResult UndoStack::undo(const std::string& project_dir,
                           const std::string& store_dir) {
    MoveResult r;
    if (refuse_foreign(r, project_dir)) {
        report_position(r);
        return r;
    }
    if (levels_.empty()) {
        r.reason = "нечего отменять: стек уровней пуст, агент ещё не "
                   "делал шагов";
        report_position(r);
        return r;
    }

    /* Снимок текущего состояния снимается ОДИН раз и до отката: после
     * отката снять уже нечего, и вернуться вперёд было бы некуда. */
    const Snapshot here = take(project_dir, store_dir);
    if (!here.ok()) {
        r.reason = "текущее состояние не снято: " + here.reason +
                   ". Переход не состоялся.";
        report_position(r);
        return r;
    }

    /* Куда идём: если проект уперёд уровня (правки текущего шага) — к
     * самому верхнему уровню, и это отменяет ровно один шаг. Если проект
     * стоит на уровне — к предыдущему, иначе второе нажатие отменило бы
     * два шага сразу. */
    const bool ahead = !same_state(here, levels_[pos_]);
    if (!ahead && pos_ == 0) {
        /* Копия, снятая ради перехода, в стек не попала: на не-git-проекте
         * это целый каталог, и «отменить» на самом раннем уровне человек
         * жмёт машинально — без уборки каждый такой press оставлял бы
         * каталог. Совпадение состояния с уровнем не делает копию чужой
         * (см. комментарий в move()). */
        trash_.push_back(here);
        r.reason = "нечего отменять: это самое раннее состояние сессии, "
                   "и с него ничего не менялось";
        report_position(r);
        return r;
    }
    return move(-1, levels_[ahead ? pos_ : pos_ - 1], here, project_dir,
                store_dir);
}

MoveResult UndoStack::redo(const std::string& project_dir,
                           const std::string& store_dir) {
    MoveResult r;
    if (refuse_foreign(r, project_dir)) {
        report_position(r);
        return r;
    }
    if (pos_ + 1 >= levels_.size()) {
        r.reason = "нечего возвращать вперёд: отменённого шага в стеке нет "
                   "(новая работа после отмены возвращает вперёд только до "
                   "этого места)";
        report_position(r);
        return r;
    }
    const Snapshot target = levels_[pos_ + 1];
    const Snapshot here = take(project_dir, store_dir);
    if (!here.ok()) {
        r.reason = "текущее состояние не снято: " + here.reason +
                   ". Переход не состоялся.";
        report_position(r);
        return r;
    }
    /* Проект упирался вперёд от отменённого уровня — то есть после отмены
     * агент что-то поменял. Возврат вперёд тогда перепрыгнул бы через
     * сделанное (правило 2 шапки), и отказ честнее: уровни впереди ещё
     * лежат в стеке, но переход через них уже не наш. */
    if (!same_state(here, levels_[pos_])) {
        trash_.push_back(here);
        r.reason = "после новых правок возвращаться вперёд нельзя: возврат "
                   "перепрыгнул бы через сделанное";
        report_position(r);
        return r;
    }
    return move(+1, target, here, project_dir, store_dir);
}

} // namespace snapshot
} // namespace coder
