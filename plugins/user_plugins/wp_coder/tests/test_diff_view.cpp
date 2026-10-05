/*
 * test_diff_view.cpp — И11.1: строки показа diff.
 *
 * Предмет проверки — РЕШЕНИЯ, а не окно: виджет рисуется ImGui, тестового
 * харнесса для него нет, и 11.13 будет сверять готовую строку и её вид.
 * Поэтому всё, что можно решить без окна, вынесено в core/diff.h и здесь
 * проверяется; ui/coder_window.cpp остаётся тонким слоем поверх.
 *
 * Откуда берутся патчи в проверках. Три источника, и все три настоящие:
 *   - `make_file_diff()` на парах строк — так патч выглядит на пути копии
 *     каталога, и это ровно тот вид, который рисует окно;
 *   - патчи, набранные руками, — для случаев, которых не бывает на живых
 *     данных: чужой заголовок hunk'а, служебная строка посреди тела,
 *     неопознанная строка. Набирать руками — не подмена, а единственный
 *     способ довести разбор до состояний, которые нельзя вызвать
 *     (то же основание, что у parse_hash, отклонение 110);
 *   - ЖИВОЙ git, для сверки номеров: разбор нашего патча обязан дать те же
 *     номера, что и разбор патча git на том же изменении. Без этого
 *     «номера считаются правильно» было бы верой о собственном коде.
 */

#include "../core/diff.h"
#include "../core/shell.h"

#include "test_framework.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace coder;
namespace fs = std::filesystem;

namespace {

std::string raw_git(const std::string& dir, const std::string& args) {
    std::string out;
    int code = 0;
    shell::run_capture_status("git -C " + shell::shell_quote(dir) + " " + args,
                              out, code, 20);
    return out;
}

/* Первая строка такой-то роли — читать проверке удобнее по смыслу, чем по
 * индексу: список строк меняет длину вместе с патчем. */
const diff::DiffRow* row_of(const std::vector<diff::DiffRow>& rows,
                            diff::DiffRowRole role, size_t n = 0) {
    size_t seen = 0;
    for (const diff::DiffRow& r : rows) {
        if (r.role != role) continue;
        if (seen == n) return &r;
        ++seen;
    }
    return nullptr;
}

size_t count_of(const std::vector<diff::DiffRow>& rows, diff::DiffRowRole role) {
    size_t n = 0;
    for (const diff::DiffRow& r : rows) {
        if (r.role == role) ++n;
    }
    return n;
}

} // namespace

/* ======================================================================
 * Разбор патча в строки: вид и номера
 * ====================================================================== */

TEST(parse_unified_gives_every_line_its_kind_and_its_numbers) {
    /* Патч снят нашим же кодом с двух состояний файла — так он выглядит
     * на пути копии каталога, и это тот же вид, который получает окно.
     * Проверяется всё, из чего строка состоит: вид, номер в своей стороне,
     * пустая колонка там, где строки нет, и заголовок hunk'а отдельной
     * строкой — иначе окно покрасило бы заголовок как текст файла. */
    const std::string before =
        "alpha\nbravo\ncharlie\ndelta\necho\nfoxtrot\ngolf\nhotel\n";
    const std::string after =
        "alpha\nBRAVO\ncharlie\ndelta\nECHO\nfoxtrot\ngolf\nhotel\n";
    const diff::FileDiff fd = diff::make_file_diff("x.txt", before, after);
    const std::vector<diff::DiffRow> rows = diff::parse_unified(fd.patch);

    ASSERT_EQ(count_of(rows, diff::DiffRowRole::Header), (size_t)1);
    ASSERT_EQ(count_of(rows, diff::DiffRowRole::Added), (size_t)2);
    ASSERT_EQ(count_of(rows, diff::DiffRowRole::Removed), (size_t)2);

    const diff::DiffRow* removed = row_of(rows, diff::DiffRowRole::Removed);
    ASSERT_TRUE(removed != nullptr);
    ASSERT_EQ(removed->text, std::string("bravo"));
    /* У удалённой строки номера только в своей стороне: колонка «после» у
     * неё пустая, и это отличает её от контекста с тем же номером. */
    ASSERT_EQ(removed->old_no, (size_t)2);
    ASSERT_EQ(removed->new_no, (size_t)0);

    const diff::DiffRow* added = row_of(rows, diff::DiffRowRole::Added);
    ASSERT_TRUE(added != nullptr);
    ASSERT_EQ(added->text, std::string("BRAVO"));
    ASSERT_EQ(added->old_no, (size_t)0);
    ASSERT_EQ(added->new_no, (size_t)2);

    const diff::DiffRow* context = row_of(rows, diff::DiffRowRole::Context);
    ASSERT_TRUE(context != nullptr);
    ASSERT_EQ(context->text, std::string("alpha"));
    ASSERT_EQ(context->old_no, (size_t)1);
    ASSERT_EQ(context->new_no, (size_t)1);
}

TEST(parse_unified_numbers_are_the_same_ours_and_gits) {
    /* ЖИВОЙ git — эталон. Наш патч и патч git на одном и том же изменении
     * должны разобраться в ОДИН И ТОТ ЖЕ номера: номера нужны человеку,
     * который ищет строку в файле, и «примерно такие» не подойдёт.
     *
     * Эталон снимается здесь, на живом репозитории, а не вписывается в
     * проверку: патч git зависит от его версии и настроек, и вписанная
     * копия перестала бы быть эталоном после первого же его изменения. */
    const fs::path dir =
        fs::temp_directory_path() / ("wp_diff_view_git_" +
                                     std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    {
        std::ofstream f(dir / "keep.txt", std::ios::binary);
        f << "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\n";
    }
    raw_git(dir.string(), "init -q");
    raw_git(dir.string(), "add -A");
    raw_git(dir.string(), "-c user.email=t@t -c user.name=t commit -q -m one");
    {
        std::ofstream f(dir / "keep.txt", std::ios::binary);
        f << "one\nTWO\nthree\nfour\nFIVE\nsix\nseven\neight\nnine\nten\n";
    }

    const std::string git_patch = raw_git(
        dir.string(), "diff --no-color --no-renames -U3 --relative");
    ASSERT_TRUE(git_patch.find("@@ ") != std::string::npos);
    /* Тело патча — всё от `--- ` (преамбула git не рисуется, отклонение
     * 132), и без завершающего перевода: так его и отдаёт окну
     * `strip_git_preamble`. */
    const size_t start = git_patch.find("--- ");
    ASSERT_TRUE(start != std::string::npos);
    std::string body = git_patch.substr(start);
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) {
        body.pop_back();
    }
    const std::vector<diff::DiffRow> git_rows = diff::parse_unified(body);

    const diff::FileDiff ours = diff::make_file_diff(
        "keep.txt",
        "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\n",
        "one\nTWO\nthree\nfour\nFIVE\nsix\nseven\neight\nnine\nten\n");
    ASSERT_TRUE(!ours.patch.empty());
    const std::vector<diff::DiffRow> our_rows = diff::parse_unified(ours.patch);

    ASSERT_EQ(our_rows.size(), git_rows.size());
    for (size_t i = 0; i < our_rows.size(); ++i) {
        ASSERT_EQ((int)our_rows[i].role, (int)git_rows[i].role);
        ASSERT_EQ(our_rows[i].text, git_rows[i].text);
        ASSERT_EQ(our_rows[i].old_no, git_rows[i].old_no);
        ASSERT_EQ(our_rows[i].new_no, git_rows[i].new_no);
    }
    fs::remove_all(dir, ec);
}

TEST(parse_unified_does_not_mistake_the_file_header_for_a_removed_line) {
    /* Ровно та ошибка, ради которой заголовок файла опознаётся ПОЗИЦИЕЙ,
     * а не видом. Удалённая строка, начинающаяся с `-`, печатается как
     * `-- …`, а заголовок файла — `--- …`: по первому символу это одно и
     * то же. Различить их можно только тем, что заголовок стоит ДО первого
     * `@@`. Ошибка здесь была бы вдвойне неприятной: в колонке номеров
     * появилась бы строка с именем файла вместо правки. */
    const std::string before = "a\n";
    const std::string after = "a\n";
    const diff::FileDiff fd = diff::make_file_diff(
        "dash.txt", before, "a\nb\n");
    ASSERT_TRUE(fd.patch.find("--- a/dash.txt") != std::string::npos);

    const std::string patch =
        "--- a/dash.txt\n"
        "+++ b/dash.txt\n"
        "@@ -1,2 +1,3 @@\n"
        " a\n"
        "--- начало раздела\n"      /* удалённая строка «--- начало…» */
        "+++ добавленная\n"         /* а эта добавленная: «++ …» */
        "+b\n";
    const std::vector<diff::DiffRow> rows = diff::parse_unified(patch);
    /* Заголовков файла в строках показа нет: имя печатает виджет один раз, а
     * в шапке патча оно и так есть. Проверяется ИХ ОТСУТСТВИЕМ, а не
     * сравнением с именем: строка заголовка хранится целиком («--- a/x»), и
     * сравнение с «a/x» прошло бы даже тогда, когда заголовок в список
     * попал — то есть проверка на это не смотрела бы вовсе. */
    ASSERT_EQ(count_of(rows, diff::DiffRowRole::Meta), (size_t)0);
    for (const diff::DiffRow& r : rows) {
        ASSERT_TRUE(r.text.compare(0, 4, "--- ") != 0);
        ASSERT_TRUE(r.text.compare(0, 4, "+++ ") != 0);
    }
    /* Пять строк: заголовок hunk'а, контекст, удаление и ДВА добавления
     * (`++ добавленная` и `+b`). Обход по числу строк ловит и утечку
     * заголовков файла: с ними строк было бы семь. */
    ASSERT_EQ(rows.size(), (size_t)5);
    ASSERT_EQ(count_of(rows, diff::DiffRowRole::Header), (size_t)1);
    ASSERT_EQ(count_of(rows, diff::DiffRowRole::Context), (size_t)1);
    ASSERT_EQ(count_of(rows, diff::DiffRowRole::Removed), (size_t)1);
    ASSERT_EQ(count_of(rows, diff::DiffRowRole::Added), (size_t)2);
    const diff::DiffRow* removed = row_of(rows, diff::DiffRowRole::Removed);
    ASSERT_TRUE(removed != nullptr);
    ASSERT_EQ(removed->text, std::string("-- начало раздела"));
    ASSERT_EQ(removed->old_no, (size_t)2);
    const diff::DiffRow* added = row_of(rows, diff::DiffRowRole::Added);
    ASSERT_TRUE(added != nullptr);
    ASSERT_EQ(added->text, std::string("++ добавленная"));
    ASSERT_EQ(added->new_no, (size_t)2);
    (void)fd;
}

TEST(parse_unified_counts_from_every_hunk_header_not_from_the_patch_start) {
    /* Два hunk'а: второй начинается со своего номера. Разбор, который
     * считал бы номера от начала патча, показал бы во втором hunk'е номера
     * первого — и человек, ищущий строку, пошёл бы не туда. */
    const std::string patch =
        "--- a/x\n"
        "+++ b/x\n"
        "@@ -1,2 +1,2 @@\n"
        " one\n"
        "-two\n"
        "+TWO\n"
        "@@ -40,2 +40,2 @@\n"
        " forty\n"
        "-fortyone\n"
        "+FORTYONE\n";
    const std::vector<diff::DiffRow> rows = diff::parse_unified(patch);
    ASSERT_EQ(count_of(rows, diff::DiffRowRole::Header), (size_t)2);
    ASSERT_EQ(count_of(rows, diff::DiffRowRole::Added), (size_t)2);
    const diff::DiffRow* second_added = row_of(rows, diff::DiffRowRole::Added, 1);
    ASSERT_TRUE(second_added != nullptr);
    ASSERT_EQ(second_added->text, std::string("FORTYONE"));
    /* Второй hunk начинается со строки 40, а не со строки 2. */
    ASSERT_EQ(second_added->old_no, (size_t)0);
    ASSERT_EQ(second_added->new_no, (size_t)41);
}

TEST(a_hunk_header_we_cannot_read_leaves_the_numbers_empty_rather_than_wrong) {
    /* Заголовок без чисел — не повод угадывать. Нумерация в этом hunk'е
     * становится нулевой (колонка пустая), а строки всё равно показаны:
     * «нет номеров» человек поймёт, «чужие номера» он бы не заметил и
     * пошёл бы искать не туда. */
    const std::string patch =
        "--- a/x\n"
        "+++ b/x\n"
        "@@ продолжение функции @@\n"
        " one\n"
        "-two\n"
        "+TWO\n";
    const std::vector<diff::DiffRow> rows = diff::parse_unified(patch);
    /* Заголовок не опознан как заголовок — он служебная строка, и это
     * видно по нему самому: с номером строки он не спутан. Но тело после
     * него всё равно тело, а не мусор: иначе каждая строка правки ушла бы
     * в Meta и колонка номеров у блока пропала бы целиком. */
    ASSERT_EQ(count_of(rows, diff::DiffRowRole::Header), (size_t)0);
    ASSERT_EQ(count_of(rows, diff::DiffRowRole::Meta), (size_t)1);
    ASSERT_EQ(count_of(rows, diff::DiffRowRole::Context), (size_t)1);
    ASSERT_EQ(count_of(rows, diff::DiffRowRole::Removed), (size_t)1);
    ASSERT_EQ(count_of(rows, diff::DiffRowRole::Added), (size_t)1);
    const diff::DiffRow* added = row_of(rows, diff::DiffRowRole::Added);
    ASSERT_TRUE(added != nullptr);
    ASSERT_EQ(added->text, std::string("TWO"));
    ASSERT_EQ(added->old_no, (size_t)0);
    ASSERT_EQ(added->new_no, (size_t)0);
}

TEST(a_line_that_is_not_a_line_of_the_file_is_shown_as_a_service_line) {
    /* Два случая, и оба обязаны быть видны. Первый — признак снятого
     * завершающего перевода (отклонение 135): выбросить его молча значило
     * бы потерять признак правки, покрасить как контекст — выдать
     * сообщение git за текст файла. Второй — строка, которую наш разбор
     * опознать не смог: она тоже не текст файла, и тоже обязана быть
     * видна, иначе патч в окне окажется короче того, что вернёт
     * `git apply`. */
    const std::string patch =
        "--- a/x\n"
        "+++ b/x\n"
        "@@ -1 +1 @@\n"
        "-a\n"
        "\\ No newline at end of file\n"
        "+a\n"
        "мусор без признака\n";
    const std::vector<diff::DiffRow> rows = diff::parse_unified(patch);
    ASSERT_EQ(count_of(rows, diff::DiffRowRole::Meta), (size_t)2);
    const diff::DiffRow* marker = row_of(rows, diff::DiffRowRole::Meta);
    ASSERT_TRUE(marker != nullptr);
    ASSERT_EQ(marker->text, std::string("\\ No newline at end of file"));
    /* Служебная строка не двигает нумерацию: относится к предыдущей. */
    ASSERT_EQ(marker->old_no, (size_t)0);
    ASSERT_EQ(marker->new_no, (size_t)0);
    const diff::DiffRow* added = row_of(rows, diff::DiffRowRole::Added);
    ASSERT_TRUE(added != nullptr);
    ASSERT_EQ(added->new_no, (size_t)1);
}

TEST(an_empty_patch_gives_no_rows_at_all) {
    /* Двоичный файл, изменение прав и обрезанный патч приходят сюда с
     * пустым `patch`, и виджет рисует для них не строки, а `note`.
     * «Пустой патч дал одну пустую строку контекста» означало бы лишнюю
     * строку под шапкой файла. */
    const std::vector<diff::DiffRow> rows = diff::parse_unified("");
    ASSERT_EQ(rows.size(), (size_t)0);
}

TEST(a_patch_ending_with_a_newline_does_not_gain_a_line_of_the_file) {
    /* Наш генератор завершающего перевода не оставляет (render_hunk и
     * strip_git_preamble его срезают), и на своих данных это недостижимо.
     * Но `parse_unified` читает то, что ему дали, а 11.2 отдаст ему патч из
     * `ToolOutput::metadata.filediff` — источник там другой.
     *
     * Проверяются ДВА случая, и они разные: завершающий перевод сам по себе
     * (обычный случай, разбор его и так переваривает) и ПУСТАЯ ПОСЛЕДНЯЯ
     * строка — два перевода подряд. Второй дал бы строку контекста, а
     * пустая строка посреди тела — это строка файла: номера уехали бы на
     * единицу, и человек искал бы правку не там. */
    const std::string patch_one =
        "--- a/x\n"
        "+++ b/x\n"
        "@@ -1 +1 @@\n"
        "-a\n"
        "+b\n";
    const std::string patch_empty_last =
        "--- a/x\n"
        "+++ b/x\n"
        "@@ -1 +1 @@\n"
        "-a\n"
        "+b\n"
        "\n";

    /* Заголовки файла в строки показа не идут, поэтому строк три:
     * заголовок hunk'а, удаление и добавление. */
    const std::vector<diff::DiffRow> one = diff::parse_unified(patch_one);
    ASSERT_EQ(one.size(), (size_t)3);
    ASSERT_EQ(count_of(one, diff::DiffRowRole::Context), (size_t)0);
    const diff::DiffRow* added = row_of(one, diff::DiffRowRole::Added);
    ASSERT_TRUE(added != nullptr);
    ASSERT_EQ(added->text, std::string("b"));
    ASSERT_EQ(added->new_no, (size_t)1);

    const std::vector<diff::DiffRow> two = diff::parse_unified(patch_empty_last);
    ASSERT_EQ(two.size(), (size_t)3);
    ASSERT_EQ(count_of(two, diff::DiffRowRole::Context), (size_t)0);
    ASSERT_EQ(count_of(two, diff::DiffRowRole::Added), (size_t)1);
}

/* ======================================================================
 * trim_diff: срезание общего ведущего отступа
 * ====================================================================== */

TEST(trim_diff_cuts_the_indent_every_line_shares) {
    /* Порт из opencode: блок, у которого все строки сдвинуты одинаково,
     * показывается от левого края, иначе половина окна уходит под отступ
     * файла, где у всех строк уровня 2-4 одинаковые два отступа. */
    const std::string patch =
        "--- a/x\n"
        "+++ b/x\n"
        "@@ -1,3 +1,3 @@\n"
        "     if (a) {\n"
        "-        b();\n"
        "+        B();\n"
        "     }\n"
        "\t\\ No newline at end of file\n";   /* служебная, но с табом */
    std::vector<diff::DiffRow> rows = diff::parse_unified(patch);
    const size_t cut = diff::trim_diff(rows);
    ASSERT_EQ(cut, (size_t)4);
    const diff::DiffRow* removed = row_of(rows, diff::DiffRowRole::Removed);
    ASSERT_TRUE(removed != nullptr);
    ASSERT_EQ(removed->text, std::string("    b();"));
    const diff::DiffRow* context = row_of(rows, diff::DiffRowRole::Context);
    ASSERT_TRUE(context != nullptr);
    ASSERT_EQ(context->text, std::string("if (a) {"));
    /* Служебные строки не трогаются. Заголовок hunk'а отступа не имеет, и
     * на нём это не видно — поэтому служебная строка здесь начинается с
     * ТАБА: табуляция это отступ, и срезание общего сдвига убрало бы её,
     * а вместе с ней и признак служебной строки. */
    const diff::DiffRow* header = row_of(rows, diff::DiffRowRole::Header);
    ASSERT_TRUE(header != nullptr);
    ASSERT_EQ(header->text, std::string("@@ -1,3 +1,3 @@"));
    const diff::DiffRow* service = row_of(rows, diff::DiffRowRole::Meta);
    ASSERT_TRUE(service != nullptr);
    ASSERT_EQ(service->text, std::string("\t\\ No newline at end of file"));
}

TEST(trim_diff_does_nothing_when_there_is_no_common_indent) {
    /* Одна строка от края — и общий отступ равен нулю. Проверка на
     * «стало меньше символов» тут была бы зелёной: обрезка происходит,
     * просто не там, где надо, и виджет показал бы блок, съехавший
     * вправо на чужую величину. */
    const std::string patch =
        "--- a/x\n"
        "+++ b/x\n"
        "@@ -1,2 +1,2 @@\n"
        "-top level\n"
        "+top level changed\n";
    std::vector<diff::DiffRow> rows = diff::parse_unified(patch);
    ASSERT_EQ(diff::trim_diff(rows), (size_t)0);
    const diff::DiffRow* removed = row_of(rows, diff::DiffRowRole::Removed);
    ASSERT_TRUE(removed != nullptr);
    ASSERT_EQ(removed->text, std::string("top level"));
}

TEST(trim_diff_keeps_blank_lines_from_deciding_the_shift) {
    /* Пустая строка посреди блока не должна обнулять сдвиг: иначе блок с
     * одним пустым рядом внутри функции показывался бы без отступа, а тот
     * же блок без пустого ряда — с ним, и выглядело бы это как правка
     * отступов, которой не было.
     *
     * Числа в проверке — те, что получаются из unified: у строки патча
     * ПЕРВЫЙ пробел — это признак контекста, а не отступ файла. Поэтому
     * «    one» в патче — это строка файла с четырьмя пробелами, и общий
     * отступ здесь четыре, а не пять. */
    const std::string patch =
        "--- a/x\n"
        "+++ b/x\n"
        "@@ -1,3 +1,3 @@\n"
        "     one\n"
        "\n"                    /* пустая строка посреди блока */
        "          two\n";      /* отступ больше общего */
    std::vector<diff::DiffRow> rows = diff::parse_unified(patch);
    ASSERT_EQ(diff::trim_diff(rows), (size_t)4);
    ASSERT_EQ(rows.size(), (size_t)4);   /* заголовок hunk'а и три строки */
    ASSERT_EQ(rows[1].text, std::string("one"));
    ASSERT_EQ(rows[2].text, std::string(""));
    /* Больше отступа, чем общий, — остаётся: срезается общий, а не весь. */
    ASSERT_EQ(rows[3].text, std::string("     two"));
}

TEST(trim_diff_never_eats_content_of_a_line_shorter_than_the_shift) {
    /* Строка, отступ которой короче общего, — это строка, состоящая из
     * одних пробелов. Срезание «общего» количества символов наивно съело
     * бы и её остаток либо вышло бы за конец строки. Общий отступ при этом
     * считают НЕ пустые строки: иначе пробельная строка обнуляла бы сдвиг
     * всего блока (см. предыдущую проверку). */
    std::vector<diff::DiffRow> rows;
    diff::DiffRow deep;
    deep.role = diff::DiffRowRole::Added;
    deep.text = "    deep";
    rows.push_back(deep);
    diff::DiffRow shallow;
    shallow.role = diff::DiffRowRole::Added;
    shallow.text = "  x";
    rows.push_back(shallow);
    diff::DiffRow blank;
    blank.role = diff::DiffRowRole::Added;
    blank.text = " ";
    rows.push_back(blank);
    ASSERT_EQ(diff::trim_diff(rows), (size_t)2);
    ASSERT_EQ(rows[0].text, std::string("  deep"));
    ASSERT_EQ(rows[1].text, std::string("x"));
    ASSERT_EQ(rows[2].text, std::string(""));
}

TEST(trim_diff_of_lines_that_are_all_blank_cuts_nothing) {
    /* Общего отступа нет, когда мерять нечего. Если бы считался и ноль,
     * «срезано 2 символа» на пустом блоке выглядело бы как правка. */
    std::vector<diff::DiffRow> rows;
    diff::DiffRow a;
    a.role = diff::DiffRowRole::Context;
    a.text = "    ";
    rows.push_back(a);
    diff::DiffRow b;
    b.role = diff::DiffRowRole::Context;
    b.text = "";
    rows.push_back(b);
    ASSERT_EQ(diff::trim_diff(rows), (size_t)0);
    ASSERT_EQ(rows[0].text, std::string("    "));
}

/* ======================================================================
 * Вид строки: цвет, ширина колонки, формат
 * ====================================================================== */

TEST(the_color_of_a_line_says_what_happened_to_it) {
    /* Тема — по роду строки, и ровно пять ролей различаются: добавленная и
     * удалённая обязаны отличаться друг от друга и от контекста, иначе
     * смысл правки читался бы только по знаку, а служебная строка была бы
     * неотличима от текста файла. */
    const diff::DiffColor added = diff::diff_role_color(diff::DiffRowRole::Added);
    const diff::DiffColor removed =
        diff::diff_role_color(diff::DiffRowRole::Removed);
    const diff::DiffColor context =
        diff::diff_role_color(diff::DiffRowRole::Context);
    const diff::DiffColor meta = diff::diff_role_color(diff::DiffRowRole::Meta);
    const diff::DiffColor header =
        diff::diff_role_color(diff::DiffRowRole::Header);
    /* Добавленная — зеленее, удалённая — краснее: сравнение каналов, а не
     * сравнение «не равны», иначе перестановка двух цветов прошла бы. */
    ASSERT_TRUE(added.g > added.r);
    ASSERT_TRUE(removed.r > removed.g);
    ASSERT_TRUE(meta.r != added.r || meta.g != added.g || meta.b != added.b);
    ASSERT_TRUE(header.r != context.r || header.g != context.g ||
                header.b != context.b);
}

TEST(the_number_column_is_wide_enough_for_the_widest_number) {
    /* Номера выравниваются в колонку, иначе «100» съедало бы знак предыдущей
     * строки, а человек читал бы правку не с того края. Узкие номера
     * держат минимум в три знака: колонка не прыгает от файла к файлу. */
    std::vector<diff::DiffRow> small;
    diff::DiffRow a;
    a.role = diff::DiffRowRole::Context;
    a.old_no = 9;
    a.new_no = 9;
    small.push_back(a);
    ASSERT_EQ(diff::diff_number_gutter(small).digits, (size_t)3);

    std::vector<diff::DiffRow> big;
    diff::DiffRow b;
    b.role = diff::DiffRowRole::Context;
    b.old_no = 1234;
    b.new_no = 99;
    big.push_back(b);
    ASSERT_EQ(diff::diff_number_gutter(big).digits, (size_t)4);
    /* Колонка — это оба номера, разделители и знак: сколько символов
     * занимает колонка целиком. */
    ASSERT_EQ(diff::diff_number_gutter(big).chars, (size_t)4 * 2 + 4);
    /* Служебная строка номера не несёт и ширину колонки не задаёт. */
    std::vector<diff::DiffRow> service;
    diff::DiffRow c;
    c.role = diff::DiffRowRole::Meta;
    service.push_back(c);
    ASSERT_EQ(diff::diff_number_gutter(service).digits, (size_t)3);
}

TEST(the_width_needed_for_a_line_without_wrapping_counts_characters_not_bytes) {
    /* Без переноса строка должна помещаться целиком, иначе конец правки
     * просто обрезается краем окна, и это худший вид молчания в виджете.
     *
     * Ширина считается в СИМВОЛАХ: кириллица в UTF-8 — два байта на букву,
     * и по байтам любая русская строка выглядела бы вдвое длиннее, и окно
     * разъезжалось бы. Служебные строки в ширину не входят: заголовок
     * hunk'а и `\ No newline at end of file` (29 символов) растягивали бы
     * окно каждого файла. */
    std::vector<diff::DiffRow> rows;
    diff::DiffRow code;
    code.role = diff::DiffRowRole::Added;
    code.text = "0123456789";
    rows.push_back(code);
    diff::DiffRow ru;
    ru.role = diff::DiffRowRole::Added;
    ru.text = "привет";   /* 6 символов, 12 байт */
    rows.push_back(ru);
    diff::DiffRow service;
    service.role = diff::DiffRowRole::Meta;
    service.text = "\\ No newline at end of file";
    rows.push_back(service);

    ASSERT_EQ(diff::diff_widest_row(rows), (size_t)10);
    const diff::DiffGutter g = diff::diff_number_gutter(rows);
    ASSERT_EQ(diff::diff_content_width(rows, g), g.chars + 10);
}

TEST(a_row_shows_both_numbers_the_sign_and_nothing_else) {
    /* Формат строки — тот же самый, что покажет окно: в 11.13 golden-снапшот
     * будет сверяться именно он, и собирать его в ui/ значило бы проверять
     * там, где проверять нечем. */
    std::vector<diff::DiffRow> rows;
    diff::DiffRow removed;
    removed.role = diff::DiffRowRole::Removed;
    removed.text = "старое";
    removed.old_no = 7;
    rows.push_back(removed);
    diff::DiffRow added;
    added.role = diff::DiffRowRole::Added;
    added.text = "новое";
    added.new_no = 8;
    rows.push_back(added);
    diff::DiffRow header;
    header.role = diff::DiffRowRole::Header;
    header.text = "@@ -6,2 +7,2 @@";
    rows.push_back(header);
    const diff::DiffGutter g = diff::diff_number_gutter(rows);
    ASSERT_EQ(g.digits, (size_t)3);

    /* У удалённой строки колонка «после» пустая: там её нет. */
    ASSERT_EQ(diff::format_diff_row(rows[0], g),
              std::string("  7     - старое"));
    ASSERT_EQ(diff::format_diff_row(rows[1], g),
              std::string("      8 + новое"));
    /* У заголовка пусты обе колонки и нет знака: он не строка файла. */
    ASSERT_EQ(diff::format_diff_row(rows[2], g),
              std::string("          @@ -6,2 +7,2 @@"));
}

TEST(a_carriage_return_at_the_end_of_a_line_is_not_drawn_as_content) {
    /* CRLF-строка отличается от LF-версии ИМЕННО завершающим CR (отклонение
     * 135), поэтому в патке он остаётся частью содержания и в номерах строк
     * он уже учтён. В тексте виджета это непечатаемый символ, который
     * ломает раскладку, поэтому нарисован он быть не может — снимается
     * ОДИН, в самом конце. CR внутри строки остаётся: он не конец строки,
     * а её содержимое, и молча выкидывать его — значило бы править файл
     * глазами. */
    diff::DiffRow row;
    row.role = diff::DiffRowRole::Removed;
    row.old_no = 1;
    row.text = "a\r";
    const diff::DiffGutter g = diff::diff_number_gutter({row});
    ASSERT_EQ(diff::format_diff_row(row, g), std::string("  1     - a"));

    diff::DiffRow inner;
    inner.role = diff::DiffRowRole::Removed;
    inner.old_no = 2;
    inner.text = "a\rb";
    ASSERT_EQ(diff::format_diff_row(inner, g), std::string("  2     - a\rb"));
}

/* ======================================================================
 * Склейка: виджет получает строки, номера, отступ и ширину из core
 * ====================================================================== */

TEST(a_trimmed_and_formatted_block_starts_at_the_left_edge) {
    /* Та же склейка, что в окне, целиком: разобрать, срезать отступ,
     * снять цвет и напечатать. Именно её 11.13 будет сверять как
     * golden-снапшот, и именно она ломается первой при ошибке в любом из
     * четырёх шагов — по отдельности каждый из них проверяется выше. */
    const std::string before =
        "    alpha\n"
        "    bravo\n"
        "    charlie\n"
        "    delta\n";
    const std::string after =
        "    alpha\n"
        "    BRAVO\n"
        "    charlie\n"
        "    delta\n";
    const diff::FileDiff fd = diff::make_file_diff("f.txt", before, after);
    std::vector<diff::DiffRow> rows = diff::parse_unified(fd.patch);
    ASSERT_EQ(diff::trim_diff(rows), (size_t)4);
    const diff::DiffGutter g = diff::diff_number_gutter(rows);

    std::string picture;
    for (const diff::DiffRow& r : rows) {
        if (!picture.empty()) picture += "\n";
        picture += diff::format_diff_row(r, g);
    }
    ASSERT_EQ(picture,
              std::string(
                  "          @@ -1,4 +1,4 @@\n"
                  "  1   1   alpha\n"
                  "  2     - bravo\n"
                  "      2 + BRAVO\n"
                  "  3   3   charlie\n"
                  "  4   4   delta"));
}
