/*
 * test_plan_consistency.cpp — согласованность AGENT_PARITY_PLAN.md с собой
 * и с кодом.
 *
 * Зачем эти проверки: план — единственный носитель состояния работы, и до
 * И4 он молча разошёлся с кодом. Таблица паритета (§1) утверждала, что
 * `glob`, `apply_patch`, `todowrite`, каскадного `edit`, `bash` со spill,
 * общего усечения и узкого окна doom-loop «отсутствуют», и держалась так
 * три итерации после И1 — потому что её правили только в части И1 и ни
 * разу после. Это тот же класс, что D2 (инструмент есть, а о нём не
 * сказано) и D12 (версия в четырёх местах), и он опаснее всего там, где
 * документ читают первым при планировании.
 *
 * Проза в журнале («закрывая итерацию, правь и §1») не работает: новая
 * сессия начинает с чтения §3 и §4 и до §1 не доходит. Поэтому каждое
 * правило здесь — механическая проверка, которая падает сама.
 *
 * Все проверки построены на двух принципах, уже применённых в этом
 * репозитории:
 *   - тест, который нельзя заставить упасть, не является проверкой: у
 *     каждой из них есть мутация, роняющая её (см. коммиты);
 *   - сообщение об ошибке называет, что делать, а не что не так.
 */

#include "test_framework.h"
#include "test_support.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace coder;

namespace {

fs::path plugin_root() {
    return fs::path(__FILE__).parent_path().parent_path();
}

std::string read_plan() {
    std::ifstream f(plugin_root() / "AGENT_PARITY_PLAN.md", std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

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

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

/* Ячейки markdown-строки: без ведущего и хвостового «|». */
std::vector<std::string> cells_of(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool started = false;
    for (char c : line) {
        if (c == '|') {
            if (started) out.push_back(trim(cur));
            cur.clear();
            started = true;
            continue;
        }
        cur += c;
    }
    if (started && !trim(cur).empty()) out.push_back(trim(cur));
    return out;
}

bool is_table_line(const std::string& line) {
    return !line.empty() && line[0] == '|';
}

/* Строка-разделитель «|---|---|» пропускаем. */
bool is_separator(const std::string& line) {
    if (!is_table_line(line)) return false;
    for (char c : line) {
        if (c != '|' && c != '-' && c != ':' && c != ' ' && c != '\r') return false;
    }
    return true;
}

/* Таблица, чей заголовок содержит header. Возвращает строки данных
 * (заголовок и разделитель пропущены) вместе с номерами строк.
 *
 * Данные читаются подряд после заголовка, пока идут строки таблицы:
 * искать заголовок в каждой строке нельзя — тогда таблица «кончается»
 * на первой же строке данных, и проверка молча ничего не находит. */
struct Row {
    std::vector<std::string> cells;
    size_t line_no = 0;
};

std::vector<Row> parse_table(const std::vector<std::string>& lines,
                             const std::string& header_contains) {
    std::vector<Row> rows;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (!is_table_line(lines[i])) continue;
        if (lines[i].find(header_contains) == std::string::npos) continue;
        for (size_t j = i + 1; j < lines.size(); ++j) {
            if (!is_table_line(lines[j])) break;
            if (is_separator(lines[j])) continue;
            Row r;
            r.cells = cells_of(lines[j]);
            r.line_no = j + 1;
            if (!r.cells.empty()) rows.push_back(std::move(r));
        }
        break;
    }
    return rows;
}

long long to_int(const std::string& s) {
    std::string t;
    for (char c : s) {
        if (c >= '0' && c <= '9') t += c;
    }
    return t.empty() ? -1 : std::stoll(t);
}

bool has_digits(const std::string& s) {
    for (char c : s) {
        if (c >= '0' && c <= '9') return true;
    }
    return false;
}

/* Кириллическая «И» — U+0418, а в UTF-8 это ДВА байта (D0 98).
 * Сравнение `s[i] == 'И'` не срабатывает никогда: литерал 'И'
 * многобайтовый, а s[i] — один байт. Такая ошибка молча даёт пустой
 * результат, и проверка проходит, ничего не проверив. */
const char* kCyrIi = "\xD0\x98";
const size_t kCyrIiLen = 2;

size_t find_cyr_ii(const std::string& s, size_t from) {
    return s.find(kCyrIi, from);
}

/* Номера итераций из произвольного текста: «И4», «И1–И3», «И4, **И9**».
 *
 * Свой сканер, а не регулярка: в `\bИ(\d{1,2})` граница слова не
 * срабатывает перед кириллицей (`\w` в ECMAScript — это [A-Za-z0-9_]),
 * и выражение молча не находило НИЧЕГО. Проверки, которые от него
 * зависели, проходили вхолостую — ровно тот случай, который эти тесты и
 * должны ловить. */
std::set<int> iterations_in(const std::string& s) {
    std::set<int> out;
    size_t i = 0;
    while (i + kCyrIiLen < s.size()) {
        const size_t at = find_cyr_ii(s, i);
        if (at == std::string::npos) break;
        i = at + kCyrIiLen;
        if (at > 0) {
            const unsigned char prev = static_cast<unsigned char>(s[at - 1]);
            if (std::isalnum(prev) || prev == '_') continue;
        }
        std::string d;
        size_t j = i;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9' && d.size() < 2)
            d += s[j++];
        if (!d.empty()) out.insert(std::stoi(d));
    }
    return out;
}

/* Тело итерации «#### ИN.» — от заголовка до следующего «####». */
std::vector<std::string> section_lines(const std::vector<std::string>& lines,
                                       size_t header_index) {
    std::vector<std::string> out;
    for (size_t i = header_index + 1; i < lines.size(); ++i) {
        if (lines[i].rfind("#### ", 0) == 0) break;
        if (lines[i].rfind("### ", 0) == 0) break;   /* конец волны */
        out.push_back(lines[i]);
    }
    return out;
}

struct Iteration {
    int number = 0;
    long long tasks = 0;
    long long done = 0;
    bool has_open_marker = false;
    bool closed_in_summary = false;
    long long summary_tasks = 0;
    long long summary_done = 0;
    size_t summary_line = 0;
};

/* Сводная таблица §4: имена и счётчики. */
std::map<int, Iteration> parse_summary(const std::vector<std::string>& lines) {
    std::map<int, Iteration> out;
    for (const Row& r : parse_table(lines, "| Итерация | Название")) {
        if (r.cells.size() < 6) continue;
        const std::set<int> nums = iterations_in(r.cells[0]);
        if (nums.size() != 1) continue;               /* ИТОГО и диапазоны */
        Iteration it;
        it.number = *nums.begin();
        it.summary_tasks = to_int(r.cells[2]);
        it.summary_done = to_int(r.cells[3]);
        it.closed_in_summary = r.cells[5].find("[x]") != std::string::npos;
        it.summary_line = r.line_no;
        out[it.number] = it;
    }
    return out;
}

} // anonymous namespace

/* ======================================================================
 * 1. Маркеры §3 == сводная таблица §4
 * ====================================================================== */

TEST(plan_task_markers_match_the_summary_table) {
    const std::string text = read_plan();
    const std::vector<std::string> lines = split_lines(text);
    std::map<int, Iteration> iters = parse_summary(lines);
    ASSERT_TRUE(!iters.empty());

    /* Считаем маркеры по секциям «#### ИN.». */
    std::map<int, Iteration> found;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].rfind("#### И", 0) != 0) continue;
        const std::set<int> nums = iterations_in(lines[i]);
        if (nums.size() != 1) continue;
        const int n = *nums.begin();
        const std::vector<std::string> body = section_lines(lines, i);
        Iteration it;
        it.number = n;
        for (const auto& l : body) {
            if (l.rfind("- [x] **", 0) == 0) ++it.done;
            if (l.rfind("- [ ] **", 0) == 0) { ++it.tasks; it.has_open_marker = true; }
        }
        it.tasks += it.done;
        found[n] = it;
    }
    ASSERT_EQ(found.size(), iters.size());

    for (auto& kv : found) {
        const int n = kv.first;
        Iteration& f = kv.second;
        ASSERT_TRUE(iters.count(n) == 1);
        const Iteration& s = iters[n];
        if (f.done != s.summary_done) {
            std::cerr << "  И" << n << ": в §3 отмечено " << f.done
                      << " выполненных задач, в сводной таблице §4 — "
                      << s.summary_done << " (строка " << s.summary_line << ")"
                      << std::endl;
        }
        ASSERT_EQ(f.done, s.summary_done);
        if (f.tasks != s.summary_tasks) {
            std::cerr << "  И" << n << ": в §3 перечислено " << f.tasks
                      << " задач, в сводной таблице §4 — " << s.summary_tasks
                      << std::endl;
        }
        ASSERT_EQ(f.tasks, s.summary_tasks);
    }
}

TEST(plan_summary_totals_are_the_sum_of_the_rows) {
    const std::string text = read_plan();
    const std::vector<std::string> lines = split_lines(text);
    long long tasks = 0, done = 0;
    long long total_tasks = -1, total_done = -1, total_percent = -1;
    for (const Row& r : parse_table(lines, "| Итерация | Название")) {
        if (r.cells.size() < 6) continue;
        bool is_total = false;
        for (const auto& c : r.cells) {
            if (c.find("ИТОГО") != std::string::npos) is_total = true;
        }
        if (is_total) {
            total_tasks = to_int(r.cells[2]);
            total_done = to_int(r.cells[3]);
            total_percent = to_int(r.cells[4]);
            continue;
        }
        if (iterations_in(r.cells[0]).size() != 1) continue;
        tasks += to_int(r.cells[2]);
        done += to_int(r.cells[3]);
    }
    ASSERT_TRUE(total_tasks > 0);
    if (tasks != total_tasks || done != total_done) {
        std::cerr << "  ИТОГО в сводной таблице не равен сумме строк: задач "
                  << tasks << " против " << total_tasks << ", готово " << done
                  << " против " << total_done << std::endl;
    }
    ASSERT_EQ(tasks, total_tasks);
    ASSERT_EQ(done, total_done);
    /* Процент обязан считаться из этих же чисел, иначе таблица врёт дважды:
     * и в итоге, и в проценте. */
    const long long computed = total_done * 100 / std::max<long long>(1, total_tasks);
    if (std::llabs(computed - total_percent) > 1) {
        std::cerr << "  процент в ИТОГО " << total_percent << " не сходится с "
                  << done << "/" << total_tasks << " = " << computed
                  << "%" << std::endl;
    }
    ASSERT_TRUE(std::llabs(computed - total_percent) <= 1);
}

TEST(plan_closed_iterations_have_every_task_done) {
    const std::string text = read_plan();
    const std::vector<std::string> lines = split_lines(text);
    for (const auto& kv : parse_summary(lines)) {
        const Iteration& it = kv.second;
        if (!it.closed_in_summary) continue;
        if (it.summary_done != it.summary_tasks) {
            std::cerr << "  И" << it.number << " помечена [x], но готово "
                      << it.summary_done << " из " << it.summary_tasks
                      << std::endl;
        }
        ASSERT_EQ(it.summary_done, it.summary_tasks);
    }
}

/* ======================================================================
 * 2. Таблица паритета §1 против закрытых итераций
 * ====================================================================== */

TEST(plan_gap_table_marks_closed_iterations_as_done) {
    const std::string text = read_plan();
    const std::vector<std::string> lines = split_lines(text);

    /* Итерация считается закрытой по §4, и отдельно — «по факту»:
     * все маркеры §3 проставлены. Второе условие ловит состояние, в
     * котором сессия проставила маркеры, но ещё не обновила сводную. */
    std::set<int> closed;
    for (const auto& kv : parse_summary(lines)) {
        if (kv.second.closed_in_summary) closed.insert(kv.first);
    }
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].rfind("#### И", 0) != 0) continue;
        const std::set<int> nums = iterations_in(lines[i]);
        if (nums.size() != 1) continue;
        const int n = *nums.begin();
        long long open = 0, total = 0;
        for (const auto& l : section_lines(lines, i)) {
            if (l.rfind("- [x] **", 0) == 0) ++total;
            if (l.rfind("- [ ] **", 0) == 0) { ++total; ++open; }
        }
        if (total > 0 && open == 0) closed.insert(n);
    }
    ASSERT_TRUE(!closed.empty());

    const std::vector<Row> gap = parse_table(lines, "| Подсистема opencode |");
    ASSERT_TRUE(!gap.empty());
    size_t checked = 0;
    for (const Row& r : gap) {
        if (r.cells.size() < 4) continue;
        const std::set<int> refs = iterations_in(r.cells[3]);
        if (refs.empty()) continue;
        /* Строка про несколько итераций проверяется, только когда закрыты
         * все: у `read` часть работы (proximity-attach) впереди. */
        bool all_closed = true;
        for (int n : refs) all_closed = all_closed && closed.count(n) == 1;
        if (!all_closed) continue;
        ++checked;
        const std::string& state = r.cells[2];
        if (state.find("✅") == std::string::npos) {
            std::cerr << "  Строка «" << r.cells[0] << "» (строка " << r.line_no
                      << ") относится к закрытой итерации "
                      << r.cells[3] << ", но в графе состояния нет ✅."
                      << " Обнови §1: это описание текущего состояния, а не"
                      << " история изменений." << std::endl;
        }
        ASSERT_TRUE(state.find("✅") != std::string::npos);
        ASSERT_TRUE(state.find("❌") == std::string::npos);
    }
    /* Если строк не нашлось вовсе — парсер разъехался, и проверка молча
     * ничего не делает (ровно тот класс, что у проверки копии манифеста,
     * указавшей на несуществующий путь). */
    ASSERT_TRUE(checked >= 10);
}

/* Список подсистем, доставленных закрытыми итерациями. Смысл: таблица
 * паритета не может «забыть» строку, потому что проверка требует, чтобы
 * каждая доставленная подсистема в ней была названа. При добавлении
 * новой строки сюда же — иначе тест останется зелёным на неполном
 * плане. */
const char* kDelivered[] = {
    "glob",
    "apply_patch",
    "todowrite",
    "edit` — каскад",
    "диспропорционального спана",
    "read` с `offset`",
    "Универсальное усечение",
    "Детектор doom-loop",
    "bash` (заменяет",
};

TEST(plan_gap_table_lists_delivered_subsystems) {
    const std::string text = read_plan();
    const std::string gap = text.substr(
        text.find("| Подсистема opencode |"),
        text.find("## 2.") - text.find("| Подсистема opencode |"));
    for (const char* name : kDelivered) {
        if (gap.find(name) == std::string::npos) {
            std::cerr << "  Подсистема «" << name << "» доставлена, но в §1"
                      << " таблицы паритета не упомянута. Добавь строку:"
                      << " подсистема | где в opencode | ✅ что сделано |"
                      << " ~~**ИN**~~" << std::endl;
        }
        ASSERT_TRUE(gap.find(name) != std::string::npos);
    }
}

/* ======================================================================
 * 3. Таблицы §4 против себя
 * ====================================================================== */

TEST(plan_deviations_table_has_a_reason_for_every_row) {
    const std::string text = read_plan();
    const std::vector<std::string> lines = split_lines(text);
    const std::vector<Row> rows =
        parse_table(lines, "| # | Было | Стало | Почему |");
    ASSERT_TRUE(!rows.empty());
    for (const Row& r : rows) {
        if (r.cells.size() < 4) continue;
        if (!has_digits(r.cells[0])) continue;
        if (r.cells[3].size() < 20) {
            std::cerr << "  Отклонение " << r.cells[0] << " (строка "
                      << r.line_no << ") не объяснено: «" << r.cells[3]
                      << "». Отклонение без причины — это не запись"
                      << " решения, а пометка." << std::endl;
        }
        ASSERT_TRUE(r.cells[3].size() >= 20);
    }
}

TEST(plan_milestones_match_their_iterations) {
    const std::string text = read_plan();
    const std::vector<std::string> lines = split_lines(text);
    std::set<int> closed;
    for (const auto& kv : parse_summary(lines)) {
        if (kv.second.closed_in_summary) closed.insert(kv.first);
    }
    const std::vector<Row> rows = parse_table(lines, "| Веха | Итерации |");
    ASSERT_TRUE(!rows.empty());
    size_t checked = 0, reached_count = 0;
    for (const Row& r : rows) {
        if (r.cells.size() < 4) continue;
        if (r.cells[0].find("M") == std::string::npos) continue;
        const std::set<int> refs = iterations_in(r.cells[1]);
        if (refs.empty()) {
            std::cerr << "  у вехи " << r.cells[0] << " (строка " << r.line_no
                      << ") не разобрана итерация: «" << r.cells[1] << "»."
                      << " Проверка молча пропустила бы её." << std::endl;
        }
        ASSERT_TRUE(!refs.empty());
        ++checked;
        const bool reached = r.cells[3].find("✅") != std::string::npos;
        if (!reached) continue;
        ++reached_count;
        for (int n : refs) {
            if (closed.count(n) == 0) {
                std::cerr << "  Веха " << r.cells[0] << " (строка " << r.line_no
                          << ") помечена достигнутой, а итерация И" << n
                          << " ещё не закрыта." << std::endl;
            }
            ASSERT_TRUE(closed.count(n) == 1);
        }
    }
    ASSERT_TRUE(checked >= 8);
    ASSERT_TRUE(reached_count >= 3);
}

TEST(plan_wave_progress_matches_the_iterations) {
    const std::string text = read_plan();
    const std::vector<std::string> lines = split_lines(text);
    std::map<int, bool> closed;   /* номер итерации → закрыта */
    std::vector<std::string> names;
    for (const Row& r : parse_table(lines, "| Итерация | Название")) {
        if (r.cells.size() < 6) continue;
        const std::set<int> nums = iterations_in(r.cells[0]);
        if (nums.size() != 1) continue;
        closed[*nums.begin()] = r.cells[5].find("[x]") != std::string::npos;
        names.push_back(r.cells[1]);
    }
    ASSERT_TRUE(closed.size() >= 14);

    /* Волны объявлены диапазонами: «(И0–И3)», «(И4–И7)», … */
    const std::vector<Row> waves = parse_table(lines, "| Волна | Содержание |");
    ASSERT_TRUE(!waves.empty());
    size_t checked = 0;
    for (const Row& w : waves) {
        if (w.cells.size() < 4) continue;
        /* Диапазон «(И0–И3)» разбираем вручную: тире здесь может быть
         * en-dash (двухбайтовый), и регулярка с байтовым классом символов
         * вела бы себя непредсказуемо. */
        const std::string& cell = w.cells[1];
        const size_t open = cell.find('(');
        const size_t close = cell.rfind(')');
        if (open == std::string::npos || close == std::string::npos || close < open)
            continue;
        const std::string inside = cell.substr(open + 1, close - open - 1);
        std::vector<int> nums;
        for (const int n : iterations_in(inside)) nums.push_back(n);
        if (nums.size() != 2) {
            std::cerr << "  у волны " << w.cells[0] << " (строка " << w.line_no
                      << ") не разобран диапазон итераций: «" << cell
                      << "». Проверка молча пропустила бы её." << std::endl;
        }
        ASSERT_TRUE(nums.size() == 2);
        const int lo = nums[0], hi = nums[1];
        size_t total = 0, done = 0;
        for (int n = lo; n <= hi; ++n) {
            if (!closed.count(n)) continue;
            ++total;
            if (closed[n]) ++done;
        }
        if (total == 0) continue;
        ++checked;
        const long long computed = done * 100 / total;
        if (std::llabs(computed - to_int(w.cells[3])) > 0) {
            std::cerr << "  Волна " << w.cells[0] << " (строка " << w.line_no
                      << "): в сводной " << computed << "% закрытых итераций, в"
                      << " таблице волн — " << w.cells[3] << std::endl;
        }
        ASSERT_EQ(computed, to_int(w.cells[3]));
        if (to_int(w.cells[2]) != (long long)total) {
            std::cerr << "  Волна " << w.cells[0] << ": итераций в диапазоне "
                      << total << ", в таблице — " << w.cells[2] << std::endl;
        }
        ASSERT_EQ(to_int(w.cells[2]), (long long)total);
    }
    ASSERT_TRUE(checked >= 4);
}

TEST(plan_readme_and_changelog_agree_on_the_version) {
    /* Версия живёт в plugin.json, копии манифеста и CHANGELOG. Три места —
     * уже повод для D12; проверка состоит в том, что CHANGELOG заголовок
     * называет текущую версию из манифеста. */
    std::ifstream f(plugin_root() / "plugin.json", std::ios::binary);
    const std::string manifest((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    const size_t v = manifest.find("\"version\"");
    ASSERT_TRUE(v != std::string::npos);
    const size_t q1 = manifest.find('"', v + 10);
    const size_t q2 = manifest.find('"', q1 + 1);
    ASSERT_TRUE(q1 != std::string::npos && q2 != std::string::npos);
    const std::string version = manifest.substr(q1 + 1, q2 - q1 - 1);

    std::ifstream c(plugin_root() / "CHANGELOG.md", std::ios::binary);
    const std::string changelog((std::istreambuf_iterator<char>(c)),
                               std::istreambuf_iterator<char>());
    ASSERT_TRUE(changelog.find("## [" + version + "]") != std::string::npos);
}
