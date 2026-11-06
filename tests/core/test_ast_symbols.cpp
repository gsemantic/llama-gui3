/*
 * test_ast_symbols.cpp — символы файла для плагина (И6.3).
 *
 * Основная часть собирает узлы руками: словарь видов и формат символа —
 * чистые функции, и проверять их через tree-sitter незачем (зависимость от
 * грамматик сделала бы проверку зелёной вхолостую там, где грамматики не
 * собраны, — ровно тот случай, о котором предупреждает SESSION_START).
 *
 * Сквозная часть (реальный файл) запускается только при TREE_SITTER_FOUND и
 * говорит об этом в имени, чтобы «грамматик нет» никогда не выглядело как
 * «проверка прошла».
 */

#include "../../src/plugins/ast_symbols.h"

#include "../test_framework.h"

#include <cstdio>
#include <fstream>
#include <string>

using namespace llama_gui::core;
using namespace llama_gui::plugin;
using json = nlohmann::json;

namespace {

AstNode node(const char* type, const char* name, int line, int end_line,
             const char* parent = "") {
    AstNode n;
    n.type = type;
    n.name = name;
    n.parent_name = parent;
    n.start_line = line;
    n.end_line = end_line;
    return n;
}

std::string write_temp(const std::string& suffix, const std::string& content) {
    const std::string path = "/tmp/wp_ast_symbols_test" + suffix;
    std::ofstream f(path, std::ios::binary);
    f << content;
    f.close();
    return path;
}

} // namespace

/* --- Словарь видов --- */

/* Вид выводится по узлу, а не отдаётся именем tree-sitter: иначе плагин
 * должен был бы знать грамматики, то есть знание о tree-sitter уехало бы
 * туда, где его быть не должно. */
void test_kind_is_derived_from_the_node_type() {
    TEST_ASSERT_EQUAL(std::string(ast_kind_for_type("function_definition")), std::string("function"));
    TEST_ASSERT_EQUAL(std::string(ast_kind_for_type("function_item")), std::string("function"));
    TEST_ASSERT_EQUAL(std::string(ast_kind_for_type("class_definition")), std::string("class"));
    TEST_ASSERT_EQUAL(std::string(ast_kind_for_type("struct_specifier")), std::string("class"));
    TEST_ASSERT_EQUAL(std::string(ast_kind_for_type("method_definition")), std::string("method"));
    TEST_ASSERT_EQUAL(std::string(ast_kind_for_type("namespace_definition")), std::string("namespace"));
    TEST_ASSERT_EQUAL(std::string(ast_kind_for_type("mod_item")), std::string("namespace"));
    TEST_ASSERT_EQUAL(std::string(ast_kind_for_type("enum_declaration")), std::string("enum"));
    TEST_ASSERT_EQUAL(std::string(ast_kind_for_type("type_definition")), std::string("typedef"));
    TEST_ASSERT_EQUAL(std::string(ast_kind_for_type("macro_definition")), std::string("macro"));
}

/* Порядок проверок: «class_definition» и «method_definition» содержат
 * «function»? Нет, но «method_declaration» содержит «method», и method
 * должен провериться раньше function — иначе метод стал бы функцией, и в
 * обзоре проекта потерялась бы принадлежность к классу. */
void test_method_wins_over_function() {
    TEST_ASSERT_EQUAL(std::string(ast_kind_for_type("method_declaration")), std::string("method"));
    TEST_ASSERT_EQUAL(std::string(ast_kind_for_type("method_definition")), std::string("method"));
}

void test_unknown_type_gets_an_explicit_kind() {
    TEST_ASSERT_EQUAL(std::string(ast_kind_for_type("weird_node")), std::string("unknown"));
}

/* --- Формат символа --- */

/* Символ несёт то, чем обзор проекта пользуется: вид, имя, родителя и
 * границы. Родитель обязателен — по нему видно, метод это или функция. */
void test_symbol_carries_kind_name_parent_and_lines() {
    const json out = symbols_to_json({node("method_definition", "run", 10, 40, "Worker")});
    TEST_ASSERT_EQUAL(out.size(), size_t(1));
    TEST_ASSERT_EQUAL(out[0]["kind"].get<std::string>(), std::string("method"));
    TEST_ASSERT_EQUAL(out[0]["name"].get<std::string>(), std::string("run"));
    TEST_ASSERT_EQUAL(out[0]["parent"].get<std::string>(), std::string("Worker"));
    TEST_ASSERT_EQUAL(out[0]["line"].get<int>(), 10);
    TEST_ASSERT_EQUAL(out[0]["end_line"].get<int>(), 40);
}

/* Узел без имени — не символ: он нечем назвать в обзоре. */
void test_node_without_name_is_not_a_symbol() {
    const json out = symbols_to_json({node("function_definition", "", 1, 2),
                                      node("function_definition", "ok", 5, 6)});
    TEST_ASSERT_EQUAL(out.size(), size_t(1));
    TEST_ASSERT_EQUAL(out[0]["name"].get<std::string>(), std::string("ok"));
}

/* Пустой список — пустой массив, а не null: вызывающий различает «файл
 * без символов» по размеру, и null сломал бы это. */
void test_empty_input_yields_an_empty_array() {
    const json out = symbols_to_json({});
    TEST_ASSERT(out.is_array());
    TEST_ASSERT_EQUAL(out.size(), size_t(0));
}

/* --- Разбор файла --- */

/* Язык определяется по расширению, когда не задан. Своей таблицы
 * расширений у моста нет намеренно: вторая разошлась бы с первой. */
void test_language_is_taken_from_the_extension() {
    const std::string path = write_temp(".cpp", "int f() { return 1; }\n");
    const json out = ast_symbols_for_file(path, "");
    TEST_ASSERT_EQUAL(out["language"].get<std::string>(), std::string("cpp"));
    std::remove(path.c_str());
}

/* Заданный язык приоритетнее: файл может прийти без расширения. */
void test_explicit_language_wins_over_the_extension() {
    const std::string path = write_temp(".txt", "x\n");
    const json out = ast_symbols_for_file(path, "python");
    TEST_ASSERT_EQUAL(out["language"].get<std::string>(), std::string("python"));
    std::remove(path.c_str());
}

/* Нефайл — отказ с ok:false. Молчаливый ok при каталоге означал бы, что
 * плагин записал в обзор «файл без символов» вместо «путь неверен».
 * Проверяется и причина, а не только факт отказа: если каталог дойдёт до
 * разбора, отказ останется, но скажет «разбор не дал узлов» — и человек
 * полчаса искал бы несуществующую ошибку грамматики. */
void test_directory_is_an_error_with_a_reason() {
    const json out = ast_symbols_for_file("/tmp", "");
    TEST_ASSERT_EQUAL(out["ok"].get<bool>(), false);
    TEST_ASSERT(out.contains("error"));
    TEST_ASSERT(out["error"].get<std::string>().find("не файл") != std::string::npos);
}

/* Ключевая проверка файла: язык без грамматики — это НЕ ошибка и НЕ пустой
 * успех, а отдельное состояние ast:false с причиной.
 *
 * Именно эту деградацию SESSION_START называет молчаливой: без грамматики
 * и без сети парсер выключается сам, и для c/cpp/python/rust падает громко,
 * а для php/js/css/html работает как ни в чём не бывало. ok:false заставил
 * бы плагина считать AST-разбор сломанным на каждом php-файле вместо того,
 * чтобы уйти на строковый скан (И13.5); ok:true с пустым symbols означал бы
 * «файл разобран, символов нет» — то есть потерю обзора проекта без единого
 * сообщения. Язык «cobol» выбран потому, что грамматики у него не бывает:
 * так состояние достигается на любой сборке, включая ту, где собраны все
 * восемь грамматик. */
void test_missing_grammar_is_a_separate_state_not_an_error() {
    const std::string path = write_temp(".cpp", "int f() { return 1; }\n");
    const json out = ast_symbols_for_file(path, "cobol");
    TEST_ASSERT_EQUAL(out["ok"].get<bool>(), true);
    TEST_ASSERT_EQUAL(out["ast"].get<bool>(), false);
    TEST_ASSERT_EQUAL(out["symbols"].size(), size_t(0));
    /* Причина обязана быть: иначе вызывающий видит ast:false и не знает,
     * падать обратно на скан или чинить сборку. */
    TEST_ASSERT(out.contains("reason"));
    TEST_ASSERT(!out["reason"].get<std::string>().empty());
    TEST_ASSERT(out["reason"].get<std::string>().find("cobol") != std::string::npos);
    std::remove(path.c_str());
}

/* Неизвестное расширение — отказ с внятной причиной, а не пустой разбор. */
void test_unknown_extension_is_an_error() {
    const std::string path = write_temp(".qqq", "x\n");
    const json out = ast_symbols_for_file(path, "");
    TEST_ASSERT_EQUAL(out["ok"].get<bool>(), false);
    TEST_ASSERT_EQUAL(out["language"].get<std::string>(), std::string(""));
    std::remove(path.c_str());
}

/* Пустой файл — НЕ отказ: грамматика есть, символов ноль. Иначе пустой файл
 * в проекте выглядел бы как сбой разбора, и его «чинили» бы. */
void test_empty_file_is_parsed_not_rejected() {
    const std::string path = write_temp(".cpp", "");
    const json out = ast_symbols_for_file(path, "");
    TEST_ASSERT_EQUAL(out["ok"].get<bool>(), true);
    TEST_ASSERT_EQUAL(out["ast"].get<bool>(), true);
    TEST_ASSERT_EQUAL(out["symbols"].size(), size_t(0));
    std::remove(path.c_str());
}

/* Файл без определений разобран успешно, и это не отказ.
 *
 * Разбор не может быть провален «тихо»: parse_source всегда возвращает узел
 * с type == "program", признака неудачи у него нет. Значит ноль символов —
 * это и «файл только с комментарием», и «файл не разобрался», и различить
 * их нечем. Объявлять отказом оба случая нельзя: ложный отказ виден, и на
 * него начинают чинить то, что не сломано. */
void test_file_without_definitions_is_parsed_successfully() {
    const std::string path = write_temp(".cpp", "// только комментарий\n/* и ещё */\n");
    const json out = ast_symbols_for_file(path, "");
    TEST_ASSERT_EQUAL(out["ok"].get<bool>(), true);
    TEST_ASSERT_EQUAL(out["ast"].get<bool>(), true);
    TEST_ASSERT_EQUAL(out["symbols"].size(), size_t(0));
    TEST_ASSERT_FALSE(out.contains("error"));
    std::remove(path.c_str());
}

/* Ключевое свойство ответа: признак ast обязан быть всегда, и он различает
 * «разобрано, символов нет» и «разобрать нечем». Плагин на этом строит
 * выбор между AST и строковым сканом (И13.5); без ast он выбрал бы молча и
 * потерял обзор проекта для php/js/css/html, не сказав об этом. */
void test_ast_flag_is_always_present() {
    const std::string path = write_temp(".cpp", "int f() { return 1; }\n");
    const json ok = ast_symbols_for_file(path, "");
    TEST_ASSERT(ok.contains("ast"));
    const json bad = ast_symbols_for_file("/tmp/nope.cpp", "");
    TEST_ASSERT(bad.contains("ast"));
    TEST_ASSERT_EQUAL(bad["ast"].get<bool>(), false);
    std::remove(path.c_str());
}

/* --- Сквозной разбор (нужна грамматика) --- */

#ifdef TREE_SITTER_FOUND
void test_real_cpp_file_yields_symbols() {
    const std::string path = write_temp(
        ".cpp",
        "#include <string>\n"
        "int helper(int x) { return x + 1; }\n"
        "struct Worker {\n"
        "  void run() {}\n"
        "};\n"
        "int main() { return helper(1); }\n");
    const json out = ast_symbols_for_file(path, "");
    if (!out["ok"].get<bool>() || !out["ast"].get<bool>()) {
        /* Грамматика заявлена как собранная, а разбор не удался — это
         * провал проверки, а не повод её пропустить. */
        TEST_ASSERT_EQUAL(out.dump(), std::string("parse failed"));
    }
    bool saw_function = false;
    bool saw_class = false;
    for (const auto& s : out["symbols"]) {
        if (s["name"].get<std::string>() == "helper") saw_function = true;
        if (s["name"].get<std::string>() == "Worker") saw_class = true;
    }
    TEST_ASSERT(saw_function);
    TEST_ASSERT(saw_class);
    std::remove(path.c_str());
}
#else
void test_real_cpp_file_yields_symbols() {
    /* Грамматики не собраны: проверка честно не выполняется и говорит об
     * этом. Молчаливый «PASSED» здесь означал бы, что разбор символов не
     * проверен никогда. */
    std::cout << "  [пропуск: tree-sitter не собран]" << std::endl;
}
#endif

int main() {
    REGISTER_TEST(test_kind_is_derived_from_the_node_type);
    REGISTER_TEST(test_method_wins_over_function);
    REGISTER_TEST(test_unknown_type_gets_an_explicit_kind);
    REGISTER_TEST(test_symbol_carries_kind_name_parent_and_lines);
    REGISTER_TEST(test_node_without_name_is_not_a_symbol);
    REGISTER_TEST(test_empty_input_yields_an_empty_array);
    REGISTER_TEST(test_language_is_taken_from_the_extension);
    REGISTER_TEST(test_explicit_language_wins_over_the_extension);
    REGISTER_TEST(test_directory_is_an_error_with_a_reason);
    REGISTER_TEST(test_missing_grammar_is_a_separate_state_not_an_error);
    REGISTER_TEST(test_unknown_extension_is_an_error);
    REGISTER_TEST(test_empty_file_is_parsed_not_rejected);
    REGISTER_TEST(test_file_without_definitions_is_parsed_successfully);
    REGISTER_TEST(test_ast_flag_is_always_present);
    REGISTER_TEST(test_real_cpp_file_yields_symbols);

    return test::TestRunner::instance().run();
}
