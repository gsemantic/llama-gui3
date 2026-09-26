/*
 * test_grammars_lock.cpp — deps/grammars.lock как единственный источник
 * грамматик tree-sitter.
 *
 * Зачем: грамматики лежат в deps/ и в git не попадают (172 МБ клонов,
 * из которых сборке нужны только src/*.c). Вместо них в репозитории
 * лежит lock-файл с восемью закреплёнными sha. Значит, протухнуть он
 * может так же незаметно, как любая документация: если он разойдётся с
 * реальными каталогами в deps/, клан соберётся с чужими грамматиками
 * и узнает об этом только на И13.5, когда `llm_ast_symbols` вдруг
 * вернёт не то.
 *
 * Проверяется ровно это: формат строк, полнота по языкам из
 * CMakeLists, длина sha, и — если каталог в deps/ оказался клоном —
 * совпадение его HEAD с записью в lock-файле. Молчание тут дороже
 * всего: неверная грамматика выглядит как «RAG просто хуже работает».
 */

#include "test_framework.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>
#include <unistd.h>

/* Намеренно без `using namespace coder`: тест ничего не вызывает из
 * плагина, он читает lock-файл и каталоги грамматик. */
namespace fs = std::filesystem;

namespace {

fs::path repo_root() {
    /* tests/ → плагин → user_plugins → plugins → корень репозитория. */
    return fs::path(__FILE__).parent_path().parent_path().parent_path()
        .parent_path().parent_path();
}

std::string read_file(const fs::path& p);

struct Entry {
    std::string lang;
    std::string url;
    std::string sha;
    size_t line_no = 0;
};

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

/* Перевод строки срезается тоже: без него путь к ref получался с
 * «\n» на конце, файл не находился, и проверка HEAD клона возвращала
 * пустоту — то есть молча пропускала расхождение версий. */
std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

/* Строка lock-файла: <язык> <url> <sha> [комментарий]. Разделитель —
 * пробелы, и это не украшение: регулярка CMake не понимает \t в классе
 * символов и не знает интервалов {n,m}, так что формат зафиксирован
 * здесь, а не в двух местах отдельно. */
bool parse_entry(const std::string& line, Entry& out) {
    const std::string t = trim(line);
    if (t.empty() || t[0] == '#') return false;
    size_t i = 0;
    while (i < t.size() && !isspace(static_cast<unsigned char>(t[i]))) ++i;
    if (i == 0 || i >= t.size()) return false;
    out.lang = t.substr(0, i);
    while (i < t.size() && isspace(static_cast<unsigned char>(t[i]))) ++i;
    size_t url_start = i;
    while (i < t.size() && !isspace(static_cast<unsigned char>(t[i]))) ++i;
    out.url = t.substr(url_start, i - url_start);
    while (i < t.size() && isspace(static_cast<unsigned char>(t[i]))) ++i;
    size_t sha_start = i;
    while (i < t.size() && !isspace(static_cast<unsigned char>(t[i]))) ++i;
    out.sha = t.substr(sha_start, i - sha_start);
    return !out.lang.empty() && !out.url.empty() && !out.sha.empty();
}

/* Языки, которые CMake просит: список живёт в CMakeLists.txt
 * (TS_LANGUAGES), и он не должен разойтись с lock-файлом. */
std::set<std::string> cmake_languages() {
    std::set<std::string> out;
    std::ifstream f(repo_root() / "CMakeLists.txt", std::ios::binary);
    std::string all((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    const std::string key = "set(TS_LANGUAGES ";
    const size_t at = all.find(key);
    if (at == std::string::npos) return out;
    /* Срез между двумя НАЙДЕННЫМИ позициями, а не посимвольный обход до
     * ')': токенайзер проглатывал саму скобку (она не пробел), и цикл
     * уезжал на остаток файла — проверка требовала языка «"» и
     * падала. Это третий подряд парсер, который съедал собственный
     * разделитель; здесь он вырезан, а не починен. */
    const size_t close = all.find(')', at + key.size());
    if (close == std::string::npos) return out;
    const std::string body = all.substr(at + key.size(), close - at - key.size());
    for (const auto& tok : split_lines(
             [&] { std::string r; for (char c : body) { if (isspace(static_cast<unsigned char>(c))) r += '\n'; else r += c; } return r; }())) {
        if (!tok.empty()) out.insert(tok);
    }
    return out;
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

bool is_hex40(const std::string& s) {
    if (s.size() != 40) return false;
    for (char c : s) {
        if (!isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

/* HEAD клона без запуска git: читаем .git/HEAD, при необходимости
 * разыменовываем ref, иначе ищем в packed-refs. Процесс здесь не
 * запускается намеренно — guard-тест на std::system в этом репозитории
 * запрещает его в исходниках, а тесту тем более. */
std::string git_head_sha(const fs::path& dir) {
    const std::string head = trim(read_file(dir / ".git" / "HEAD"));
    if (head.empty()) return "";
    if (head.rfind("ref:", 0) != 0) return head;   /* detached HEAD */
    const std::string ref = trim(head.substr(4));
    const std::string loose = read_file(dir / ".git" / ref);
    const std::string t = trim(loose);
    if (is_hex40(t)) return t;
    /* ref в packed-refs: «<sha> <ref>» построчно. */
    const std::vector<std::string> packed =
        split_lines(read_file(dir / ".git" / "packed-refs"));
    for (const auto& l : packed) {
        if (l.empty() || l[0] == '#' || l[0] == '^') continue;
        const size_t sp = l.find(' ');
        if (sp == std::string::npos) continue;
        if (trim(l.substr(sp + 1)) == ref) return trim(l.substr(0, sp));
    }
    return "";
}

} // anonymous namespace

TEST(grammars_lock_file_exists_and_parses) {
    const fs::path lock = repo_root() / "deps" / "grammars.lock";
    ASSERT_TRUE(fs::exists(lock));
    const std::vector<std::string> lines = split_lines(read_file(lock));
    size_t parsed = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        Entry e;
        const std::string t = trim(lines[i]);
        if (t.empty() || t[0] == '#') continue;
        if (!parse_entry(lines[i], e)) {
            std::cerr << "  не разобрана строка " << (i + 1) << " lock-файла: «"
                      << t << "»" << std::endl;
        }
        ASSERT_TRUE(parse_entry(lines[i], e));
        ++parsed;
    }
    /* Ноль строк данных — тот же случай, что был с проверкой копии
     * манифеста: парсер разъехался, и проверка молча ничего не делает. */
    ASSERT_TRUE(parsed >= 8);
}

TEST(grammars_lock_covers_every_language_cmake_asks_for) {
    const std::set<std::string> wanted = cmake_languages();
    /* Список из CMakeLists.txt обязан читаться: иначе проверка ниже
     * пропустит всё, не увидев ни одного языка. */
    ASSERT_TRUE(wanted.size() >= 8);

    std::set<std::string> in_lock;
    const std::vector<std::string> lines =
        split_lines(read_file(repo_root() / "deps" / "grammars.lock"));
    for (const auto& l : lines) {
        Entry e;
        if (parse_entry(l, e)) in_lock.insert(e.lang);
    }
    for (const auto& lang : wanted) {
        if (in_lock.count(lang) == 0) {
            std::cerr << "  CMake просит «" << lang
                      << "», а в deps/grammars.lock его нет: клон останется"
                         " без AST-разбора, и тест ast_parser упадёт"
                      << std::endl;
        }
        ASSERT_TRUE(in_lock.count(lang) == 1);
    }
}

TEST(grammars_lock_pins_full_sha_and_https_url) {
    const std::vector<std::string> lines =
        split_lines(read_file(repo_root() / "deps" / "grammars.lock"));
    size_t checked = 0;
    for (const auto& l : lines) {
        Entry e;
        if (!parse_entry(l, e)) continue;
        ++checked;
        /* Полный sha, а не тег и не ветка: тег upstream переставляют, и
         * тогда клон молча собрал бы другую грамматику. */
        if (!is_hex40(e.sha)) {
            std::cerr << "  «" << e.lang << "»: sha «" << e.sha
                      << "» — нужен ровно 40 шестнадцатеричных символов"
                      << std::endl;
        }
        ASSERT_TRUE(is_hex40(e.sha));
        ASSERT_TRUE(e.url.rfind("https://", 0) == 0);
    }
    ASSERT_TRUE(checked >= 8);
}

TEST(grammars_lock_matches_the_grammars_on_disk) {
    /* Если каталог в deps/ — клон, его HEAD обязан совпадать с записью.
     * Расхождение означает, что машина собирается на одной грамматике,
     * а клон из репозитория получит другую: и это всплывёт не здесь, а
     * там, где результат разбора уже нельзя будет сверить. */
    const std::vector<std::string> lines =
        split_lines(read_file(repo_root() / "deps" / "grammars.lock"));
    std::set<std::string> compared;
    for (const auto& l : lines) {
        Entry e;
        if (!parse_entry(l, e)) continue;
        const fs::path dir =
            repo_root() / "deps" / ("tree-sitter-" + e.lang + "-src");
        if (!fs::exists(dir / ".git")) continue;   /* распакованная копия */
        if (!fs::exists(dir / "src" / "parser.c")) {
            std::cerr << "  «" << e.lang << "»: клон есть, а src/parser.c нет —"
                      << " это не грамматика" << std::endl;
            ASSERT_TRUE(fs::exists(dir / "src" / "parser.c"));
        }
        const std::string head = git_head_sha(dir);
        if (head.empty()) {
            std::cerr << "  «" << e.lang
                      << "»: не удалось прочитать HEAD клона" << std::endl;
        }
        ASSERT_TRUE(!head.empty());
        compared.insert(e.lang);
        if (head != e.sha) {
            std::cerr << "  «" << e.lang << "»: в deps/ HEAD " << head
                      << ", а в lock-файле " << e.sha
                      << ". Соберится не то, что проверено." << std::endl;
        }
        ASSERT_TRUE(head == e.sha);
    }
    /* На этой машине клоны есть хотя бы для веб-стека; если проверка не
     * сравнила ничего — она ничего и не доказывает. */
    if (fs::exists(repo_root() / "deps" / "tree-sitter-php-src" / ".git"))
        ASSERT_TRUE(compared.count("php") == 1);
}
