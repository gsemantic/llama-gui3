#include "../../include/core/document_parser.h"
#include "../../include/core/ast_parser.h"
#include "../test_framework.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace llama_gui::core;

namespace {

std::string temp_path(const std::string& name) {
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("llama_gui_doc_parser_" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    return (dir / name).string();
}

void write_file(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

bool contains(const std::vector<std::string>& paragraphs, const std::string& needle) {
    for (const auto& paragraph : paragraphs) {
        if (paragraph.find(needle) != std::string::npos) return true;
    }
    return false;
}

std::string join(const std::vector<std::string>& paragraphs) {
    std::string result;
    for (const auto& paragraph : paragraphs) {
        result += paragraph;
        result += '\n';
    }
    return result;
}

} // namespace

// ============================================================================
// Mallard (.page)
// ============================================================================

void test_parse_mallard_page() {
    const std::string path = temp_path("topic.page");
    write_file(path,
               "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
               "<page xmlns=\"http://projectmallard.org/1.0/\" type=\"topic\">\n"
               "  <info><desc>Краткое описание <app>Swell Foop</app>.</desc></info>\n"
               "  <title>Введение</title>\n"
               "  <p>Первый <em>абзац</em> справки.</p>\n"
               "  <p>Второй абзац.</p>\n"
               "</page>\n");

    const auto paragraphs = DocumentParser::parse_document(path);

    TEST_ASSERT(paragraphs.size() >= 3);
    TEST_ASSERT(contains(paragraphs, "Введение"));
    TEST_ASSERT(contains(paragraphs, "Первый абзац справки."));
    TEST_ASSERT(contains(paragraphs, "Второй абзац."));
    TEST_ASSERT(contains(paragraphs, "Краткое описание Swell Foop."));
    TEST_ASSERT_FALSE(contains(paragraphs, "<p>"));
}

// ============================================================================
// DocBook (.docbook) with internal entities
// ============================================================================

void test_parse_docbook_entities() {
    const std::string path = temp_path("manual.docbook");
    write_file(path,
               "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
               "<!DOCTYPE article PUBLIC \"-//OASIS//DTD DocBook XML V4.1.2//EN\" "
               "\"docbookx.dtd\" [\n"
               "  <!ENTITY appname \"GParted\">\n"
               "  <!ENTITY ver \"1.6\">\n"
               "]>\n"
               "<article><articleinfo><title>&appname; &ver;</title></articleinfo>\n"
               "<section><title>Разделы</title>\n"
               "<para>Программа &appname; умеет работать с разделами.</para>\n"
               "</section></article>\n");

    const auto paragraphs = DocumentParser::parse_document(path);

    TEST_ASSERT(contains(paragraphs, "GParted 1.6"));
    TEST_ASSERT(contains(paragraphs, "Программа GParted умеет работать с разделами."));
    TEST_ASSERT_FALSE(contains(paragraphs, "&appname;"));
}

// ============================================================================
// HTML: tags, comments, script/style, entities
// ============================================================================

void test_parse_html_markup() {
    const std::string path = temp_path("help.html");
    write_file(path,
               "<!DOCTYPE html>\n"
               "<html><head><title>Тест &amp; проверка</title>\n"
               "<style>body { color: red; }</style>\n"
               "<script>var x = \"не текст\";</script></head>\n"
               "<body><h1>Заголовок</h1>\n"
               "<p>Первый &laquo;абзац&raquo; и &#1055;&#x440;&#1080;&#1074;&#1077;&#1090;.</p>\n"
               "<!-- комментарий не должен попасть -->\n"
               "</body></html>\n");

    const auto paragraphs = DocumentParser::parse_document(path);
    const std::string text = join(paragraphs);

    TEST_ASSERT(contains(paragraphs, "Тест & проверка"));
    TEST_ASSERT(contains(paragraphs, "Заголовок"));
    TEST_ASSERT(contains(paragraphs, "Первый «абзац» и Привет."));
    TEST_ASSERT_FALSE(text.find("color: red") != std::string::npos);
    TEST_ASSERT_FALSE(text.find("не текст") != std::string::npos);
    TEST_ASSERT_FALSE(text.find("комментарий") != std::string::npos);
}

// ============================================================================
// Unsupported extension
// ============================================================================

void test_unsupported_extension_returns_empty() {
    const std::string path = temp_path("data.unsupported");
    write_file(path, "some text");

    const auto paragraphs = DocumentParser::parse_document(path);

    TEST_ASSERT_TRUE(paragraphs.empty());
}

// ============================================================================
// R0.4: HTML/CSS — язык, тип и AST-чанкинг
// ============================================================================

void test_get_language_html_css() {
    TEST_ASSERT_EQUAL(DocumentParser::get_language("page.html"), "html");
    TEST_ASSERT_EQUAL(DocumentParser::get_language("page.htm"), "html");
    TEST_ASSERT_EQUAL(DocumentParser::get_language("style.css"), "css");
    /* Язык не зависит от регистра расширения: файл мог прийти с Windows. */
    TEST_ASSERT_EQUAL(DocumentParser::get_language("PAGE.HTML"), "html");
    /* Незнакомое расширение — пустой язык, а не угаданный. */
    TEST_ASSERT_EQUAL(DocumentParser::get_language("data.unsupported"), "");
}

void test_detect_content_type_html_is_text() {
    /* .html остаётся ТЕКСТОМ: документация читается как документ, а не как
     * код. Иначе RAG индексировал бы куски с сырыми тегами, а
     * test_parse_html_markup (script/style/комментарии не в тексте) начал
     * бы падать. Язык при этом "html" — это разные вопросы. */
    TEST_ASSERT(DocumentParser::detect_content_type("page.html") ==
                DocumentParser::ContentType::Text);
    TEST_ASSERT(DocumentParser::detect_content_type("page.htm") ==
                DocumentParser::ContentType::Text);
}

void test_detect_content_type_css_is_code() {
    /* CSS как документ не читается (нет прозы), зато как код даёт правила
     * с селекторами и номерами строк. */
    TEST_ASSERT(DocumentParser::detect_content_type("style.css") ==
                DocumentParser::ContentType::Code);
    /* Проверка «соседних» расширений: контрпример к «/code для всего». */
    TEST_ASSERT(DocumentParser::detect_content_type("notes.txt") ==
                DocumentParser::ContentType::Text);
}

void test_parse_code_html_ast() {
    /* Если html-грамматика собрана и слинкована — AST-чанки обязаны иметь
     * symbol_name и номера строк. Если нет — parse_code деградирует, и
     * проверять надо деградацию, а не падать на «грамматики нет».
     *
     * Условие берётся из AstParser::is_available, а не из #ifdef: в
     * сборке без tree-sitter тест не должен становиться зелёным на пустом
     * результате, он должен честно сказать «проверять нечего». */
    const std::string path = temp_path("page.html");
    write_file(path,
        "<html><head><title>T</title></head>\n"
        "<body><h1>Заголовок</h1><p>Абзац</p></body></html>\n");

    const auto chunks = DocumentParser::parse_code(path);

    if (!AstParser::is_available("html")) {
        /* Деградация: чанки есть, но это текст/эвристика без symbol_name.
         * Главное — не пустой список и не падение. */
        TEST_ASSERT(!chunks.empty());
        return;
    }

    TEST_ASSERT(!chunks.empty());

    bool found_h1 = false;
    bool found_p = false;
    for (const auto& c : chunks) {
        /* Формат AST-чанка: [[start:end:name:parent:file]]content.
         * Полей ЧЕТЫРЕ, и имя — третье, а не «всё после второго двоеточия»
         * (иначе в name попадёт "h1:body:/tmp/..." и сравнение провалится). */
        if (c.rfind("[[", 0) != 0) continue;
        const size_t close = c.find("]]");
        TEST_ASSERT(close != std::string::npos);
        const std::string meta = c.substr(2, close - 2);

        std::vector<std::string> fields;
        size_t start = 0;
        while (true) {
            const size_t colon = meta.find(':', start);
            if (colon == std::string::npos) { fields.push_back(meta.substr(start)); break; }
            fields.push_back(meta.substr(start, colon - start));
            start = colon + 1;
        }
        /* start:end:name[:parent[:file]] — минимум три поля */
        TEST_ASSERT(fields.size() >= 3);
        const std::string& name = fields[2];
        if (name == "h1") found_h1 = true;
        if (name == "p") found_p = true;
    }
    TEST_ASSERT_TRUE(found_h1);
    TEST_ASSERT_TRUE(found_p);
}

void test_parse_code_css_falls_back_without_grammar() {
    /* css-грамматика (v15) отсекается CMake как несовместимая с рантаймом,
     * поэтому parse_code обязан деградировать, а не падать и не отдавать
     * пустоту. Это ровно тот путь, на котором раньше был SIGSEGV:
     * set_language возвращал 0, а результат не проверялся. */
    const std::string path = temp_path("style.css");
    write_file(path, "body { color: red; }\n.card { margin: 0; }\n");

    const auto chunks = DocumentParser::parse_code(path);

    if (!AstParser::is_available("css")) {
        /* Эвристика split_code_by_blocks для неизвестного языка: блоки
         * есть, но без symbol_name. Не пусто — значит деградация
         * рабочая. */
        TEST_ASSERT(!chunks.empty());
        return;
    }

    TEST_ASSERT(!chunks.empty());
}

int main() {
    REGISTER_TEST(test_parse_mallard_page);
    REGISTER_TEST(test_parse_docbook_entities);
    REGISTER_TEST(test_parse_html_markup);
    REGISTER_TEST(test_unsupported_extension_returns_empty);

    REGISTER_TEST(test_detect_content_type_html_is_text);
    REGISTER_TEST(test_detect_content_type_css_is_code);
    REGISTER_TEST(test_get_language_html_css);
    REGISTER_TEST(test_parse_code_html_ast);
    REGISTER_TEST(test_parse_code_css_falls_back_without_grammar);

    int result = test::TestRunner::instance().run();

    std::error_code ec;
    std::filesystem::remove_all(
        std::filesystem::temp_directory_path() /
            ("llama_gui_doc_parser_" + std::to_string(getpid())),
        ec);

    return result;
}
