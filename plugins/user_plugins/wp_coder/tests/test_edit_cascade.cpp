/*
 * test_edit_cascade.cpp — И4.3: каскад из девяти стадий замены.
 *
 * Проверяется не «работает ли каскад», а три вещи, которые ломаются
 * молча:
 *   1. каждая стадия действительно применима к своему случаю (иначе
 *      каскад — декорация: восемь стадий, всегда срабатывает первая);
 *   2. единственность соблюдается НА КАЖДОЙ стадии — «нашлось в двух
 *      местах» это отказ, а не выбор первого;
 *   3. нечёткая стадия (BlockAnchor) честно называет себя в отчёте: правка
 *      по сходству обязана быть видна пользователю.
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/text_edit.h"
#include "../core/json.h"
#include "../core/tool.h"
#include "../core/tools_registry.h"
#include "../core/engine.h"
#include "../core/base_tools.h"
#include "../core/project.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace coder;

namespace {

fs::path make_tmp_tree() {
    fs::path tmp = fs::temp_directory_path()
        / ("wp_coder_edit_" + std::to_string(::getpid()) + "_"
           + std::to_string(std::rand()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    return tmp;
}

void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

void init_tools(const fs::path& project) {
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&, LlmReply&) { return false; };
    cb.llm_complete = [](const std::string&, const std::string&, std::string&) { return false; };
    cb.llm_is_connected = []() { return false; };
    cb.chat_event = [](const std::string&) {};
    Engine::instance().init(cb);
    test_support::approve_all_permissions();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = project.string();
        engine_state().allowed_external_paths.clear();
        engine_state().plan_mode = false;
        engine_state().pending.clear();
    }
    register_base_tools();
}

EditResult edit(const std::string& file_text, const std::string& old_text,
                const std::string& new_text, bool replace_all = false) {
    EditRequest req;
    req.old_text = old_text;
    req.new_text = new_text;
    req.replace_all = replace_all;
    return apply_edit(split_text(file_text), req);
}

std::string run_replace(const std::string& path, const std::string& old_text,
                        const std::string& new_text, bool replace_all = false) {
    json::JsonValue args = json::JsonValue::object();
    args.set("path", path);
    args.set("query", old_text);
    args.set("content", new_text);
    if (replace_all) args.set("replace_all", true);
    return ToolsRegistry::instance().run("search_replace", args);
}

} // anonymous namespace

/* ======================================================================
 * Стадии по одной
 * ====================================================================== */

TEST(cascade_simple_stage_matches_literally) {
    EditResult r = edit("a\nb\nc\n", "b", "B");
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(join_text(r.file), std::string("a\nB\nc\n"));
    ASSERT_TRUE(r.used == EditStrategy::Simple);
    /* Номер строки — для отчёта и будущего diff. */
    ASSERT_EQ(r.line, (size_t)2);
}

TEST(cascade_line_trimmed_stage_ignores_trailing_spaces) {
    /* Пробелы в конце строк приносит МОДЕЛЬ (окно из редактора). В файле их
     * нет — иначе «return 1;» нашлось бы как обычная подстрока и проверять
     * было бы нечего. */
    EditResult r = edit("if (x) {\n    return 1;\n}\n", "    return 1;   ", "    return 2;");
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.used == EditStrategy::LineTrimmed);
    ASSERT_EQ(join_text(r.file),
              std::string("if (x) {\n    return 2;\n}\n"));
}

TEST(cascade_block_anchor_stage_succeeds_on_typo) {
    /* Модель опечаталась в имени функции: отказ здесь означал бы, что
     * агент застревает на правке, которую человек сделал бы с первого
     * раза. Порог подобия — 0.65 (И4.3). */
    EditResult r = edit("function loadUser() {\n  return 1;\n}\n",
                        "function loadUsr() {\n  return 1;\n}",
                        "function fetchUser() {\n  return 1;\n}");
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.used == EditStrategy::BlockAnchor);
    ASSERT_EQ(join_text(r.file),
              std::string("function fetchUser() {\n  return 1;\n}\n"));
}

TEST(cascade_block_anchor_stage_refuses_unrelated_text) {
    /* Порог 0.65 не должен превращаться в «замени похожее на похожее»:
     * разные слова — это не опечатка. */
    EditResult r = edit("alpha\nbeta\n", "totally\ndifferent", "x");
    ASSERT_FALSE(r.ok);
    ASSERT_TRUE(r.error.find("не найден") != std::string::npos);
}

TEST(cascade_whitespace_normalized_stage_tolerates_blank_lines) {
    EditResult r = edit("x = 1;\na = 2;\nb = 3;\n", "x = 1;\n\na = 2;", "y = 1;\nb = 2;");
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.used == EditStrategy::WhitespaceNormalized);
    ASSERT_EQ(join_text(r.file), std::string("y = 1;\nb = 2;\nb = 3;\n"));
}

TEST(cascade_indentation_flexible_stage_ignores_leading_spaces) {
    /* Отступ большой, и блок многострочный: иначе до этой стадии не дойдёт
     * ни Simple (однострочный запрос с другим отступом — обычная
     * подстрока), ни BlockAnchor (при небольшой разнице отступов он
     * срабатывает раньше, стадия 3). */
    EditResult r = edit("aaa\n        bbb\nccc\n", "aaa\nbbb\nccc", "AAA\nbbb\nCCC");
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.used == EditStrategy::IndentationFlexible);
    /* Соседние строки не склеились: регион блока съел переводы строк,
     * и замена обязана их вернуть. */
    ASSERT_EQ(join_text(r.file), std::string("AAA\nbbb\nCCC\n"));
}

TEST(cascade_escape_normalized_stage_decodes_escapes) {
    /* Модель прислала «\\n» вместо перевода строки — классика мелких
     * моделей. Файл при этом обычный. */
    EditResult r = edit("line one\nline two\n", "line one\\nline two", "merged");
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.used == EditStrategy::EscapeNormalized);
    ASSERT_EQ(join_text(r.file), std::string("merged\n"));
}

TEST(cascade_escape_normalized_stage_works_on_escaped_file_content) {
    /* Обратный случай: файл JSON хранит перевод строки как «\\n», и
     * буквального совпадения в нём нет. */
    EditResult r = edit("{\"a\": \"x\\ny\"}\n", "x\\ny", "z");
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(join_text(r.file), std::string("{\"a\": \"z\"}\n"));
}

TEST(cascade_trimmed_boundary_stage_ignores_surrounding_whitespace) {
    /* Пробелы по краям присланного фрагмента — частый артефакт fenced-блока
     * в ответе модели. */
    EditResult r = edit("code\nfoo\n", "  code  \n", "CODE");
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.used == EditStrategy::TrimmedBoundary);
    ASSERT_EQ(join_text(r.file), std::string("CODE\nfoo\n"));
}

TEST(cascade_context_aware_stage_drops_one_mismatched_context_line) {
    /* Модель приклеила лишнюю строку (хвост fenced-блока), а её нет в
     * файле. Человек давно бы стёр лишнее; каскад делает это за него, но
     * только когда оставшееся место единственное. */
    EditResult r = edit("one\ntwo\nthree\n", "one\ntwo\nЛИШНЯЯ", "ONE TWO");
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.used == EditStrategy::ContextAware);
    ASSERT_EQ(join_text(r.file), std::string("ONE TWO\nthree\n"));

    /* Отброшенная строка может быть и первой. */
    EditResult first = edit("one\ntwo\n", "ЛИШНЯЯ\ntwo", "TWO");
    ASSERT_TRUE(first.ok);
    ASSERT_EQ(join_text(first.file), std::string("one\nTWO\n"));
}

TEST(cascade_context_aware_refuses_to_drop_many_lines) {
    /* Совпадение по одной строке при нескольких несовпавших — это не
     * правка, а подмена нужного на ненужное. */
    EditResult r = edit("one\ntwo\n", "one\nДРУГАЯ\nЕЩЁ\nthree", "X");
    ASSERT_FALSE(r.ok);
}

TEST(cascade_multi_occurrence_requires_explicit_flag) {
    const std::string src = "return 1;\nreturn 1;\nreturn 1;\n";
    /* Без флага — отказ с числом вхождений: ровно как раньше. */
    EditResult refused = edit(src, "return 1;", "return 2;");
    ASSERT_FALSE(refused.ok);
    ASSERT_TRUE(refused.error.find("3 одинаковых вхождени") != std::string::npos);
    ASSERT_TRUE(refused.error.find("replace_all") != std::string::npos);

    EditResult all = edit(src, "return 1;", "return 2;", true);
    ASSERT_TRUE(all.ok);
    ASSERT_TRUE(all.used == EditStrategy::MultiOccurrence);
    ASSERT_EQ(all.occurrences, (size_t)3);
    ASSERT_EQ(join_text(all.file),
              std::string("return 2;\nreturn 2;\nreturn 2;\n"));
}

/* ======================================================================
 * Инварианты каскада
 * ====================================================================== */

TEST(cascade_keeps_uniqueness_on_every_stage) {
    /* Два одинаковых блока: ни одна стадия не имеет права выбрать
     * «первый» — это правка не там, и заметить её можно только по diff. */
    const std::string src = "function a() {\n  return 1;\n}\n"
                            "function b() {\n  return 1;\n}\n";
    /* Simple: точное совпадение блока-подстроки встречается дважды. */
    EditResult r1 = edit(src, "  return 1;\n}", "  return 2;\n}");
    ASSERT_FALSE(r1.ok);
    /* BlockAnchor: два места, похожие на присланное с опечаткой. */
    EditResult r2 = edit("value_one\nvalue_two\nvalue_one\nvalue_two\n",
                         "value_on", "VALUE_ONE");
    ASSERT_FALSE(r2.ok);
    ASSERT_TRUE(r2.error.find("вхождени") != std::string::npos);
}

TEST(cascade_block_anchor_does_not_swallow_the_rest_of_a_line) {
    /* Модель прислала ФРАГМЕНТ строки, встречающийся один раз. Замена идёт
     * точно, по фрагменту: нечёткая стадия работала бы по строкам целиком и
     * вычеркнула бы «+ 1». */
    EditResult fuzzy = edit("total = value + 1\n", "value", "VALUE");
    ASSERT_TRUE(fuzzy.ok);
    ASSERT_TRUE(fuzzy.used != EditStrategy::BlockAnchor);
    ASSERT_EQ(join_text(fuzzy.file), std::string("total = VALUE + 1\n"));
    /* А когда фрагмент встречается ровно один раз, замена идёт точно и
     * ровно по фрагменту, а не по строке. */
    EditResult two = edit("total = value + 1\nvalue\n", "value", "VALUE");
    ASSERT_FALSE(two.ok);
    ASSERT_TRUE(two.error.find("2 одинаковых вхождени") != std::string::npos);
}

TEST(cascade_reports_which_stage_worked) {
    /* Стадия — не деталь реализации: по ней видно, насколько правка
     * «точная», и это же объясняет модель, почему сработало. */
    ASSERT_TRUE(edit("a\nb\n", "b", "B").used == EditStrategy::Simple);
    ASSERT_TRUE(edit("b\n", "b   ", "B").used == EditStrategy::LineTrimmed);
    ASSERT_TRUE(edit("a\nb\n", "a\n\nb", "a\nb").used ==
                EditStrategy::WhitespaceNormalized);
    ASSERT_EQ(std::string(edit_strategy_name(EditStrategy::BlockAnchor)),
              std::string("blockAnchor"));
}

TEST(cascade_preserves_file_format_while_editing) {
    /* Правка в Windows-файле не должна превратить его в Unix-файл: иначе
     * git показывает изменение всего файла. */
    const std::string raw = "\xEF\xBB\xBF" "a;\r\nb;\r\n";
    EditResult r = edit(raw, "b;", "B;");
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(join_text(r.file), std::string("\xEF\xBB\xBF" "a;\r\nB;\r\n"));
}

TEST(cascade_rejects_empty_query) {
    EditResult r = edit("a\n", "", "b");
    ASSERT_FALSE(r.ok);
    ASSERT_TRUE(r.error.find("пустой") != std::string::npos);
}

TEST(cascade_keeps_file_unchanged_when_nothing_matches) {
    /* Отказ не должен оставлять наполовину заменённый файл. */
    EditResult r = edit("a\nb\nc\n", "совершенно другое\nи это", "x");
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(join_text(r.file), std::string("a\nb\nc\n"));
}

TEST(line_similarity_is_bounded_and_cheap) {
    ASSERT_TRUE(line_similarity("return 1;", "return 1;") == 1.0);
    ASSERT_TRUE(line_similarity("return 1;", "return 2;") > 0.65);
    ASSERT_TRUE(line_similarity("return 1;", "SELECT * FROM users") < 0.65);
    /* Первая буква другая — непохожие, расстояние не считается впустую. */
    ASSERT_TRUE(line_similarity("alpha", "zzzzzzzzzzzzzzzz") == 0.0);
    /* Очень длинные строки не сравниваются вовсе: расстояние Левенштейна
     * квадратично, и файл из minified-кода повесил бы правку. Совпадающие
     * строки отвечают сразу — это дешёвый путь, а не обход предохранителя. */
    ASSERT_TRUE(line_similarity(std::string(4000, 'a'),
                                std::string(4000, 'a')) == 1.0);
    ASSERT_TRUE(line_similarity(std::string(4000, 'a'),
                                std::string(3999, 'a') + "b") == 0.0);
}

TEST(cascade_does_not_hang_on_minified_file) {
    /* Тот же предохранитель на реальном файле: одна длинная строка и
     * фрагмент из неё. Правка обязана либо пройти точно, либо отказать —
     * но не считать расстояние по миллиону операций на строку. */
    const std::string minified = std::string(3000, 'x') + "needle" +
                                 std::string(3000, 'y');
    EditResult r = edit(minified + "\n", "needle", "NEEDLE");
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.used == EditStrategy::BlockAnchor, false);
    ASSERT_TRUE(join_text(r.file).find("NEEDLE") != std::string::npos);
}

/* ======================================================================
 * И4.4 — защита от непропорционального спана
 * ====================================================================== */

TEST(cascade_refuses_disproportionate_span) {
    /* Модель не нашла нужное место и прислала «весь файл» вместо трёх строк.
     * Молчаливая перезапись чужого кода видна только в diff — поэтому отказ. */
    std::string whole;
    for (int i = 0; i < 40; ++i) whole += "line " + std::to_string(i) + "\n";
    EditResult r = edit(whole, whole, "line 1\n");
    ASSERT_FALSE(r.ok);
    ASSERT_TRUE(r.error.find("непропорциональный спан") != std::string::npos);
    /* Сообщение actionable: сказано, что делать. */
    ASSERT_TRUE(r.error.find("разбей") != std::string::npos);

    /* Граница формулы: блок в 2 раза больше замены — уже отказ, а блок,
     * который лишь немного больше, проходит. */
    std::string five = "1\n2\n3\n4\n5\n6\n";
    EditResult big = edit(five, "1\n2\n3\n4\n5\n", "x\n");
    ASSERT_FALSE(big.ok);
    EditResult small = edit(five, "1\n2\n3\n", "x\n");
    ASSERT_TRUE(small.ok);
}

TEST(cascade_allows_growing_a_file_by_a_lot) {
    /* Обратное направление — законная вставка: две строки на двести. */
    std::string filler;
    for (int i = 0; i < 60; ++i) filler += "generated line " + std::to_string(i) + "\n";
    EditResult r = edit("a\nb\n", "b", filler);
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(join_text(r.file).find("generated line 59") != std::string::npos);
}

TEST(cascade_span_check_can_be_disabled_for_whole_file_rewrite) {
    /* Явная перезапись файла (write_file) — законная операция, и каскад
     * должен позволять её: проверку выключает вызывающий, а не пользователь. */
    std::string whole;
    for (int i = 0; i < 20; ++i) whole += "x" + std::to_string(i) + "\n";
    EditRequest req;
    req.old_text = whole;
    req.new_text = "short\n";
    req.max_span_ratio = 0;
    EditResult r = apply_edit(split_text(whole), req);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(join_text(r.file), std::string("short\n"));
}

/* ======================================================================
 * Инструмент
 * ====================================================================== */

TEST(search_replace_writes_and_reports_the_stage) {
    fs::path t = make_tmp_tree();
    write_file(t / "a.php", "<?php\n    return 1;\n");
    init_tools(t);
    /* Пробелы в конце строки пришли от модели — из-за них Simple и не
     * срабатывает, иначе проверять было бы нечего. */
    std::string r = run_replace("a.php", "    return 1;   ", "    return 2;");
    ASSERT_TRUE(r.find("строка 2") != std::string::npos);
    ASSERT_TRUE(r.find("lineTrimmed") != std::string::npos);
    ASSERT_EQ(read_file(t / "a.php"), std::string("<?php\n    return 2;\n"));
    fs::remove_all(t);
}

TEST(search_replace_warns_about_fuzzy_match) {
    fs::path t = make_tmp_tree();
    write_file(t / "a.php", "function loadUser() {\n  return 1;\n}\n");
    init_tools(t);
    std::string r = run_replace("a.php", "function loadUsr() {\n  return 1;\n}",
                                "function fetchUser() {\n  return 1;\n}");
    /* Пользователь обязан знать, что правка легла по сходству. */
    ASSERT_TRUE(r.find("ВНИМАНИЕ") != std::string::npos);
    ASSERT_TRUE(r.find("blockAnchor") != std::string::npos);
    ASSERT_EQ(read_file(t / "a.php"),
              std::string("function fetchUser() {\n  return 1;\n}\n"));
    fs::remove_all(t);
}

TEST(search_replace_multiple_occurrences_ask_for_context) {
    fs::path t = make_tmp_tree();
    write_file(t / "a.php", "return 1;\nreturn 1;\n");
    init_tools(t);
    std::string r = run_replace("a.php", "return 1;", "return 2;");
    ASSERT_TRUE(r.find("2 одинаковых вхождени") != std::string::npos);
    ASSERT_EQ(read_file(t / "a.php"), std::string("return 1;\nreturn 1;\n"));
    /* С явным флагом — заменяются все, и это сказано в отчёте. */
    std::string r2 = run_replace("a.php", "return 1;", "return 2;", true);
    ASSERT_TRUE(r2.find("2 вхождени") != std::string::npos);
    ASSERT_EQ(read_file(t / "a.php"), std::string("return 2;\nreturn 2;\n"));
    fs::remove_all(t);
}

TEST(search_replace_proposes_in_plan_mode) {
    fs::path t = make_tmp_tree();
    write_file(t / "a.php", "a\nb\n");
    init_tools(t);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().plan_mode = true;
    }
    std::string r = run_replace("a.php", "a", "A");
    ASSERT_TRUE(r.find("предложено") != std::string::npos);
    ASSERT_EQ(read_file(t / "a.php"), std::string("a\nb\n"));
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        ASSERT_EQ(engine_state().pending.size(), (size_t)1);
        ASSERT_EQ(engine_state().pending[0].path, std::string("a.php"));
        ASSERT_EQ(engine_state().pending[0].content, std::string("A\nb\n"));
        engine_state().plan_mode = false;
        engine_state().pending.clear();
    }
    fs::remove_all(t);
}

TEST(search_replace_keeps_crlf_on_disk) {
    fs::path t = make_tmp_tree();
    write_file(t / "win.txt", "a\r\nb\r\n");
    init_tools(t);
    std::string r = run_replace("win.txt", "b", "B");
    ASSERT_TRUE(r.find("[ошибка]") == std::string::npos);
    /* Байт-в-байт: CRLF обязан уцелеть, иначе diff покажет весь файл. */
    ASSERT_EQ(read_file(t / "win.txt"), std::string("a\r\nB\r\n"));
    fs::remove_all(t);
}
