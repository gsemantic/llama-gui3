/*
 * test_diff.cpp — И10.5: построчный diff и unified-разметка.
 *
 * Тут нет ни git, ни файловой системы: сравниваются строки и печатается
 * патч. Это не «упрощение проверки», а её предмет: всё, что здесь
 * проверяется, — форма, а форма обязана совпасть с той, что печатает
 * git, иначе человек увидит в окне нечто, чего никогда не видел в
 * `git diff`.
 *
 * Совпадение с git проверяется НАСТОЯЩИМ git: эталонный патч берётся
 * командой в временном репозитории и сверяется с нашим ПОСИМВОЛЬНО.
 * Пока этого нет, «наш unified похож на git-овский» было бы верой, а
 * верётся она проверкой или не проверяется вовсе. Четыре свойства git,
 * на которые здесь опёрто, сняты с живого репозитория ДО правки кода:
 *   - перемещённый блок печатается как одна вставка и одно удаление
 *     (общая подпоследовательность ищется);
 *   - CRLF и BOM — часть содержимого строки: смена переводов меняет
 *     КАЖДУЮ строку, а BOM — первую;
 *   - снятие завершающего перевода строки печатается как `-b` / `+b`
 *     плюс `\ No newline at end of file`;
 *   - имя файла в заголовке бывает трёх форм, и две из них несовместимы
 *     (кавычки с побегами против табуляции-разделителя).
 */

#include "../core/diff.h"
#include "../core/limits.h"
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

void write_file(const fs::path& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
}

std::string raw_git(const std::string& dir, const std::string& args) {
    std::string out;
    int code = 0;
    shell::run_capture_status("git -C " + shell::shell_quote(dir) + " " + args,
                              out, code, 20);
    return out;
}

/* ТЕЛО unified-патча: всё от строки `--- ` до конца, без преамбулы git
 * (`diff --git a/… b/…`, `index …`) и без хвоста `@@ … @@ <function>`,
 * который git дописывает по своей эвристике имени функции.
 *
 * Именно тело и есть unified-формат: его печатает `diff -u` и его умеет
 * разбирать наш apply_patch. Преамбулу и хвост печатает git, и наш
 * генератор их не печатает — это НАЗВАННОЕ различие, а не дефект:
 * окно рисует строки патча, а не заголовок git. Сравнивать целиком
 * «наш патч с патчем git» было бы сравнением разных вещей: первая
 * версия проверки так и делала, и падала на `diff --git`. */
std::string patch_body(std::string git_patch) {
    const size_t start = git_patch.find("--- ");
    if (start == std::string::npos) return std::string();
    git_patch = git_patch.substr(start);
    if (!git_patch.empty() && git_patch.back() == '\n') git_patch.pop_back();
    /* Хвост заголовка hunk'а: всё после第二个 @@ в строке заголовка. */
    size_t pos = 0;
    while ((pos = git_patch.find("@@ -", pos)) != std::string::npos) {
        const size_t close = git_patch.find("@@", pos + 3);
        if (close == std::string::npos) break;
        const size_t line_end = git_patch.find('\n', close);
        const size_t tail_end = line_end == std::string::npos ? git_patch.size()
                                                             : line_end;
        git_patch.erase(close + 2, tail_end - (close + 2));
        pos = close + 2;
    }
    return git_patch;
}

size_t count_lines(const std::string& text) {
    size_t n = 0;
    for (char c : text) {
        if (c == '\n') ++n;
    }
    return n;
}

/* Каталог под живой git-эталон. Копирование запрещено: деструктор
 * удаляет каталог, а копия фикстуры — это отложенная удалка (тот же
 * класс, что в test_snapshot.cpp). */
struct RepoBox {
    fs::path root;
    RepoBox(const RepoBox&) = delete;
    RepoBox& operator=(const RepoBox&) = delete;
    explicit RepoBox(const char* what) {
        root = fs::temp_directory_path() /
               ("wp_coder_diff_" + std::string(what) + "_" +
                std::to_string(::getpid()));
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);
        raw_git(root.string(), "init -q .");
        raw_git(root.string(), "config user.email test@example.com");
        raw_git(root.string(), "config user.name test");
        raw_git(root.string(), "config commit.gpgsign false");
    }
    ~RepoBox() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    /* Хеш БЕЗ перевода строки, и это не мелочь: `rev-parse` печатает
     * его с переводом, а команда собирается строкой — перевод внутри неё
     * сделал бы вторую строку отдельной командой, и git отвечал бы
     * «not found» на собственный хеш. Ровно это и случилось в первой
     * версии проверки. */
    std::string commit() {
        raw_git(root.string(), "add -A");
        raw_git(root.string(), "commit -q -m c");
        std::string out = raw_git(root.string(), "rev-parse HEAD^{tree}");
        const size_t nl = out.find('\n');
        if (nl != std::string::npos) out.resize(nl);
        return out;
    }
};

} // namespace

TEST(diff_lines_marks_only_the_lines_that_changed) {
    /* Самая частая правка и самая частая ошибка: diff, который помечает
     * файл целиком. Человек увидел бы «переписано всё» и не смог бы
     * отличить одну изменённую строку от двадцати. */
    const std::vector<std::string> before = {"a", "b", "c", "d", "e"};
    const std::vector<std::string> after = {"a", "b", "C", "d", "e"};
    const std::vector<diff::DiffLine> lines = diff::diff_lines(before, after);

    size_t removed = 0, added = 0, context = 0;
    for (const diff::DiffLine& l : lines) {
        if (l.kind == diff::LineKind::Removed) {
            ++removed;
            ASSERT_EQ(l.text, std::string("c"));
            /* Номер строки в своей стороне: у удалённой строки номера
             * «после» нет, и наоборот — иначе подсветка в UI (11.1)
             * показывала бы номера не с той стороны. */
            ASSERT_EQ(l.old_no, (size_t)3);
            ASSERT_EQ(l.new_no, (size_t)0);
        }
        if (l.kind == diff::LineKind::Added) {
            ++added;
            ASSERT_EQ(l.text, std::string("C"));
            ASSERT_EQ(l.new_no, (size_t)3);
            ASSERT_EQ(l.old_no, (size_t)0);
        }
        if (l.kind == diff::LineKind::Context) ++context;
    }
    ASSERT_EQ(removed, (size_t)1);
    ASSERT_EQ(added, (size_t)1);
    ASSERT_EQ(context, (size_t)4);
    ASSERT_EQ(lines.size(), (size_t)6);
}

TEST(diff_lines_treats_a_moved_block_the_way_git_does) {
    /* Переехавший блок печатается как ОДНА вставка и ОДНО удаление —
     * так же, как это делает живой git на том же примере (проверено до
     * правки). Ожидание «3 удаления и 3 вставки» было бы верно для
     * сравнения без поиска общей подпоследовательности, и проверка
     * тогда ловила бы не наш diff, а недосмотр алгоритма. */
    const std::vector<std::string> before = {"h", "1", "2", "3", "f"};
    const std::vector<std::string> after = {"h", "f", "1", "2", "3"};
    const std::vector<diff::DiffLine> lines = diff::diff_lines(before, after);
    size_t removed = 0, added = 0;
    for (const diff::DiffLine& l : lines) {
        if (l.kind == diff::LineKind::Removed) ++removed;
        if (l.kind == diff::LineKind::Added) ++added;
    }
    ASSERT_EQ(removed, (size_t)1);
    ASSERT_EQ(added, (size_t)1);
    /* Начало совпадает, и совпадение идёт ДО вставки: если бы алгоритм
     * не искал общую подпоследовательность, «h» остался бы один, а
     * дальше пошёл бы сплошной перебор. */
    ASSERT_TRUE(lines.front().kind == diff::LineKind::Context);
    ASSERT_EQ(lines.front().text, std::string("h"));
}

TEST(the_patch_has_exactly_the_form_git_prints) {
    /* ГЛАВНАЯ проверка файла: наш патч и патч git совпадают ПОСИМВОЛЬНО
     * на изменении одной строки. Пока это не проверено, «формат тот же»
     * — это утверждение в комментарии, которое ничего не проверяет, и
     * подсветка 11.1 рисовала бы по разметке, которой человек никогда
     * не видел.
     *
     * Эталон берётся живым git с тем же флагом `-U3`, то есть с тем же
     * числом строк контекста. */
    RepoBox box("shape");
    const std::string before =
        "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\n";
    const std::string after =
        "one\ntwo\nthree\nfour\nFIVE\nsix\nseven\neight\nnine\nten\n";
    write_file(box.root / "f.txt", before);
    const std::string t1 = box.commit();
    write_file(box.root / "f.txt", after);
    const std::string t2 = box.commit();

    const std::string git_patch = patch_body(raw_git(
        box.root.string(), "diff --no-color --no-renames -U3 " + t1 + " " + t2));
    const diff::FileDiff fd = diff::make_file_diff("f.txt", before, after);

    ASSERT_EQ(fd.additions, (size_t)1);
    ASSERT_EQ(fd.deletions, (size_t)1);
    ASSERT_FALSE(fd.binary);
    ASSERT_FALSE(fd.truncated);
    ASSERT_TRUE(fd.note.empty());
    ASSERT_EQ(fd.patch, git_patch);
    /* Номера строк в заголовке hunk'а обязаны быть номерами СТОРОН:
     * здесь меняется пятая строка, и两边 диапазона — 1..7. */
    /* Диапазон начинается не с единицы: сверху три строки контекста,
     * поэтому первая показанная строка четвёртая, и git печатает
     * `@@ -2,7 +2,7 @@` — ровно это и сравнивается выше посимвольно. */
    ASSERT_TRUE(fd.patch.find("@@ -2,7 +2,7 @@") != std::string::npos);
    ASSERT_TRUE(fd.patch.find("-five\n+FIVE\n") != std::string::npos);
}

TEST(the_patch_splits_far_apart_changes_into_two_hunks) {
    /* Две правки в разных концах файла — это ДВА hunk'а. Склеенный в
     * один hunk показал бы двадцать строк контекста как правку, и
     * человек подумал бы, что файл переписан. */
    std::string before, after;
    for (int i = 0; i < 20; ++i) {
        before += "line" + std::to_string(i) + "\n";
        after += (i == 2 || i == 17) ? "CHANGED" + std::to_string(i) + "\n"
                                     : "line" + std::to_string(i) + "\n";
    }
    const diff::FileDiff fd = diff::make_file_diff("f.txt", before, after);
    ASSERT_EQ(fd.additions, (size_t)2);
    ASSERT_EQ(fd.deletions, (size_t)2);
    size_t hunks = 0;
    for (size_t pos = fd.patch.find("@@ -"); pos != std::string::npos;
         pos = fd.patch.find("@@ -", pos + 1)) {
        ++hunks;
    }
    ASSERT_EQ(hunks, (size_t)2);
    ASSERT_TRUE(fd.patch.find("@@ -1,6 +1,6 @@") != std::string::npos);
    ASSERT_TRUE(fd.patch.find("@@ -15,6 +15,6 @@") != std::string::npos);
}

TEST(a_lost_final_newline_is_shown_as_a_change_of_the_last_line) {
    /* Проверено на живом git: снятие завершающего перевода печатается
     * как `-b` / `+b` плюс признак `\ No newline at end of file`. Без
     * такой пары строк патч был бы пустым при непустой разнице байтов,
     * то есть «правок нет» при правке. */
    const diff::FileDiff fd = diff::make_file_diff("nonl.txt", "a\nb\n", "a\nb");
    ASSERT_EQ(fd.additions, (size_t)1);
    ASSERT_EQ(fd.deletions, (size_t)1);
    ASSERT_TRUE(fd.patch.find("-b\n+b\n\\ No newline at end of file") !=
                std::string::npos);
    /* Обратный случай: перевод ЕСТЬ с обеих сторон — признака нет. */
    const diff::FileDiff other = diff::make_file_diff("f.txt", "a\nb\n", "a\nB\n");
    ASSERT_TRUE(other.patch.find("\\ No newline") == std::string::npos);
}

TEST(a_crlf_file_is_shown_the_way_git_shows_it) {
    /* CRLF — это НЕ «изменений нет» и не «изменилось две строки».
     * Живой git на смене переводов показывает изменение КАЖДОЙ строки
     * (`-a^M` / `+a`), и строки патча заканчиваются `\n`, а `\r`
     * остаётся частью содержимого строки. Нормализовать переводы перед
     * сравнением было бы «удобно» и неверно: человек увидел бы в окне
     * почти пустой diff и нажал бы «отменить», потеряв настоящую
     * правку либо не заметив, что изменились все строки.
     *
     * Эталон — живой git, посимвольно. */
    RepoBox box("crlf");
    const std::string before = "a\r\nb\r\nc\r\n";
    const std::string after = "a\nB\nc\n";
    write_file(box.root / "f.txt", before);
    const std::string t1 = box.commit();
    write_file(box.root / "f.txt", after);
    const std::string t2 = box.commit();

    const std::string git_patch = patch_body(raw_git(
        box.root.string(), "diff --no-color --no-renames -U3 " + t1 + " " + t2));
    const diff::FileDiff fd = diff::make_file_diff("f.txt", before, after);
    ASSERT_EQ(fd.patch, git_patch);
    /* Меняются ВСЕ три строки, а не одна: у каждой строки CRLF-файла
     * перевод стал частью содержимого, и git показывает три удаления и
     * три вставки. Ожидание «1 и 1» было бы верно для нормализованного
     * сравнения, то есть проверяло бы неверную версию кода — а
     * посимвольное сравнение с патчем git выше такой вариант отсекает. */
    ASSERT_EQ(fd.additions, (size_t)3);
    ASSERT_EQ(fd.deletions, (size_t)3);
}

TEST(a_bom_shows_up_as_a_change_of_the_first_line) {
    /* BOM — часть ПЕРВОЙ строки, и git показывает именно так: `-p` /
     * `+\xEF\xBB\xBFp`. Версия «проигнорировать BOM, он же не
     * содержание» дала бы пустой diff при изменённом файле, а человек
     * решил бы, что файл не трогали. */
    const std::string before = "p\nq\n";
    const std::string after = "\xEF\xBB\xBF" "p\nq\n";
    const diff::FileDiff fd = diff::make_file_diff("bom.txt", before, after);
    ASSERT_EQ(fd.additions, (size_t)1);
    ASSERT_EQ(fd.deletions, (size_t)1);
    ASSERT_TRUE(fd.patch.find("+\xEF\xBB\xBFp") != std::string::npos);
}

TEST(a_binary_file_is_not_reported_as_a_file_without_changes) {
    /* «Двоичный файл» и «изменений нет» — это разные вещи, и второе
     * было бы враньём: человек увидел бы «правок нет» и оставил файл
     * как есть, а потом удивился бы, что изменение не откатилось. */
    const std::string before = std::string("a\0b", 3) + "\n";
    const std::string after = std::string("a\0c", 3) + "\n";
    const diff::FileDiff fd = diff::make_file_diff("bin.dat", before, after);
    ASSERT_TRUE(fd.binary);
    ASSERT_EQ(fd.additions, (size_t)0);
    ASSERT_EQ(fd.deletions, (size_t)0);
    ASSERT_TRUE(fd.patch.empty());
    ASSERT_TRUE(fd.note.find("двоичный") != std::string::npos);
    /* Признак ставится по NUL в начале файла — тем же правилом, что у
     * git, иначе один и тот же файл смотрелся бы то как текст, то как
     * двоичный. */
    ASSERT_FALSE(diff::looks_binary("обычный текст\n"));
    /* NUB дальше начала окна — это уже не двоичный файл по правилу git
     * (он читает первые 8000 байт). Проверка на «NUL где-то в файле»
     * объявила бы двоичными файлы, которые git показывает как текст. */
        /* NUL дальше окна в 8000 байт — это НЕ двоичный файл по правилу git
     * (он читает начало файла). Проверка на «NUL где-то в файле»
     * объявила бы двоичными файлы, которые git показывает как текст.
     *
     * NUL приписывается через std::string(1, '\0'), а НЕ через строковый
     * литерал "\0": у литерала длина ноль (`strlen("\0") == 0`), то есть
     * в прошлой версии проверки к строке из девяти тысяч 'x' НЕ
     * добавлялось ничего, и проверка была зелёной на пустоте. Её
     * поймала мутация «двоичным считается любой NUL», а не чтение. */
    ASSERT_FALSE(diff::looks_binary(std::string(9000, 'x') + std::string(1, '\0')));
    ASSERT_TRUE(diff::looks_binary(std::string(10, 'x') + std::string(1, '\0')));
    /* Граница окна — 8000 байт: NUL ровно на её краю уже двоичный. */
    ASSERT_TRUE(diff::looks_binary(std::string(7999, 'x') + std::string(1, '\0')));
}

TEST(parse_git_header_name_reads_the_forms_git_prints) {
    /* Формы сняты с живого git 2.39, и две из них несовместимы: имя с
     * кавычкой или не-ASCII печатается в кавычках с восьмеричными
     * побегами, а имя с ПРОБЕЛОМ — без кавычек и с табуляцией в конце
     * строки. Табуляция и есть разделитель, поэтому «просто убрать
     * a/» не работает. */
    ASSERT_EQ(diff::parse_git_header_name("a/keep.txt"), std::string("keep.txt"));
    ASSERT_EQ(diff::parse_git_header_name("b/sub/deep.txt"),
              std::string("sub/deep.txt"));
    ASSERT_EQ(diff::parse_git_header_name("a/ascii space.txt\t"),
              std::string("ascii space.txt"));
    ASSERT_EQ(diff::parse_git_header_name("\"a/q\\\"uote.php\""),
              std::string("q\"uote.php"));
    /* \320\276\320\261\320\202 — это байты слова «обы» в восьмеричном
     * виде: такие байты git печатает для не-ASCII.
     *
     * Ожидание задано БАЙТАМИ, а не буквами, и это не формальность:
     * кириллица в строковом литерале проверки один раз соврала сама
     * (заглавные «ОБ» вместо строчных), и проверка падала на РАВНЫХ
     * байтах — то есть сравнение текста видело разницу там, где
     * её нет. Байты от такой ошибки свободны. */
    const std::string kObyy = "\320\276\320\261\320\202";
    ASSERT_EQ(diff::parse_git_header_name("\"a/\\320\\276\\320\\261\\320\\202\""),
              kObyy);
    /* У нового файла имени на этой стороне нет — это /dev/null, а не
     * файл с именем «dev/null». */
    ASSERT_TRUE(diff::parse_git_header_name("/dev/null").empty());
    /* Удалённый файл: имя есть в `---`, а в `+++` стоит /dev/null. */
    ASSERT_EQ(diff::parse_git_header_name("a/gone.txt"), std::string("gone.txt"));
}

TEST(parse_git_header_name_refuses_what_it_cannot_read) {
    /* Отказ вместо догадки. Имя файла — это адрес, и подобранное на
     * глаз имя показало бы diff чужого файла: человек прочитал бы
     * правку одного файла на файле с другим именем. */
    /* Пробел БЕЗ разделяющей табуляции: имя не кончилось, поле
     * заголовка неоднозначно. */
    ASSERT_TRUE(diff::parse_git_header_name("a/file b.txt").empty());
    /* Незакрытая кавычка, неизвестный побег, оборванный побег. */
    ASSERT_TRUE(diff::parse_git_header_name("\"a/unterminated").empty());
    ASSERT_TRUE(diff::parse_git_header_name("\"a/unknown\\q\"").empty());
    ASSERT_TRUE(diff::parse_git_header_name("\"a/short\\9\"").empty());
    /* Префикса a/ или b/ нет — это не наш заголовок. */
    ASSERT_TRUE(diff::parse_git_header_name("keep.txt").empty());
    ASSERT_TRUE(diff::parse_git_header_name("").empty());
    ASSERT_TRUE(diff::parse_git_header_name("\t").empty());
}

TEST(a_file_too_big_to_compare_precisely_is_replaced_whole_and_says_so) {
    /* На файле, где изменено почти всё, таблица сравнения не
     * помещается в предел. Тогда показывается замена целиком — и это
     * ОБЪЯЗАНО сказано: грубая правка, показанная как точная, заставила
     * бы человека нажать «отменить» не разобравшись. */
    std::string before, after;
    const size_t n = 1100;   /* 1100 x 1100 = 1 210 000 > 1 000 000 */
    for (size_t i = 0; i < n; ++i) {
        before += "old line " + std::to_string(i) + "\n";
        after += "new line " + std::to_string(i) + "\n";
    }
    const diff::FileDiff fd = diff::make_file_diff("big.txt", before, after);
    ASSERT_EQ(fd.additions, n);
    ASSERT_EQ(fd.deletions, n);
    ASSERT_TRUE(fd.note.find("замена целиком") != std::string::npos);
    /* Обрезка по пределу строк отрезает хвост, а удаления в unified
     * идут ПЕРЕД добавлениями, поэтому в показанном начале первых
     * добавлений нет. Проверяется именно это: иначе «показана замена
     * целиком» осталось бы недоказанным — обрезка могла бы съесть
     * начало, и человек увидел бы только удаления. */
    ASSERT_TRUE(fd.patch.find("-old line 0") != std::string::npos);
    ASSERT_TRUE(fd.patch.find("+new line 0") == std::string::npos);
    /* Предел показа соблюдён, иначе окно зависло бы на том, ради чего
     * предел и введён. */
    ASSERT_TRUE(count_lines(fd.patch) <= limits::kMaxDiffLinesPerFile + 3);
    ASSERT_TRUE(fd.truncated);
}

TEST(a_file_that_is_compared_exactly_says_nothing_about_roughness) {
    /* Обратная сторона предыдущей проверки: на файле, который сравнился
     * точно, note о грубой замене появляться НЕ должен. Иначе человек
     * читал бы «показана замена целиком» под точной правкой и не
     * доверял бы ни одной цифре. */
    std::string before, after;
    for (size_t i = 0; i < 1100; ++i) {
        before += "line " + std::to_string(i) + "\n";
        after += (i == 5) ? "CHANGED\n" : "line " + std::to_string(i) + "\n";
    }
    const diff::FileDiff fd = diff::make_file_diff("ok.txt", before, after);
    ASSERT_EQ(fd.additions, (size_t)1);
    ASSERT_EQ(fd.deletions, (size_t)1);
    ASSERT_TRUE(fd.note.empty());
    ASSERT_FALSE(fd.truncated);
}

TEST(a_long_patch_is_cut_and_announces_the_cut) {
    /* Обрезанный патч без объявления — молчаливая ложь: человек увидел
     * бы начало правки и решил, что это вся правка. */
    std::string before, after;
    for (size_t i = 0; i < limits::kMaxDiffLinesPerFile + 50; ++i) {
        before += "same " + std::to_string(i) + "\n";
        after += "changed " + std::to_string(i) + "\n";
    }
    const diff::FileDiff fd = diff::make_file_diff("long.txt", before, after);
    ASSERT_TRUE(fd.truncated);
    ASSERT_TRUE(fd.note.find("первые") != std::string::npos);
    ASSERT_TRUE(count_lines(fd.patch) <= limits::kMaxDiffLinesPerFile + 3);
    /* Счётчики остаются полными: обрезан только показ, и «+900/−900»
     * человек всё равно должен увидеть целиком. */
    ASSERT_EQ(fd.additions, limits::kMaxDiffLinesPerFile + 50);
}

TEST(an_identical_file_produces_no_patch_and_says_why) {
    /* Пустой патч без причины читался бы как «сравнение не
     * состоялось». Различие обязано быть названо. */
    const diff::FileDiff fd = diff::make_file_diff("f.txt", "a\nb\n", "a\nb\n");
    ASSERT_TRUE(fd.patch.empty());
    ASSERT_EQ(fd.additions, (size_t)0);
    ASSERT_EQ(fd.deletions, (size_t)0);
    ASSERT_TRUE(!fd.note.empty());
}
