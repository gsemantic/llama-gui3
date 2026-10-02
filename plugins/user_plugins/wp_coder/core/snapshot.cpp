/*
 * snapshot.cpp — снимок состояния рабочего каталога (И10.1).
 *
 * Обоснование выбора команд и границы честности — в шапке snapshot.h;
 * здесь только код. Два места, где легко ошибиться, и оба закрыты
 * комментарием при коде:
 *
 *   - `git stash create` при чистом дереве отдаёт ПУСТОЙ вывод, а при
 *     конфликте — текст ошибки непустым. Поэтому «непустой вывод» не
 *     может быть признаком успеха: успех — это нулевой код возврата И
 *     хеш в ответе.
 *   - `git stash create` не берёт неотслеживаемые файлы (см. шапку), и
 *     `git write-tree` молча отдаёт индекс, то есть тоже не видит новых
 *     файлов. Это свойство git; обманывать его нечем.
 */

#include "core/snapshot.h"

#include "core/limits.h"
#include "core/shell.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <system_error>

namespace coder {
namespace snapshot {

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

} // namespace snapshot
} // namespace coder