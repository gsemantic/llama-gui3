#include "../../include/core/document_parser.h"
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

int main() {
    REGISTER_TEST(test_parse_mallard_page);
    REGISTER_TEST(test_parse_docbook_entities);
    REGISTER_TEST(test_parse_html_markup);
    REGISTER_TEST(test_unsupported_extension_returns_empty);

    int result = test::TestRunner::instance().run();

    std::error_code ec;
    std::filesystem::remove_all(
        std::filesystem::temp_directory_path() /
            ("llama_gui_doc_parser_" + std::to_string(getpid())),
        ec);

    return result;
}
