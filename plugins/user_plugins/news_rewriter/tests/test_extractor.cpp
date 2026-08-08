#include "test_framework.h"

#include <string>

#include "extractor.h"

using namespace news_rewriter;

namespace {

static void test_html_to_text_strips_tags() {
    const std::string html = "<html><body><p>Привет, <b>мир</b>!</p></body></html>";
    const std::string text = html_to_text(html);
    TEST_ASSERT_EQUAL(text, "Привет, мир!");
}

static void test_html_to_text_block_breaks() {
    const std::string html = "<p>Первый абзац.</p><p>Второй абзац.</p>";
    const std::string text = html_to_text(html);
    TEST_ASSERT(text.find("Первый абзац.") != std::string::npos);
    TEST_ASSERT(text.find("Второй абзац.") != std::string::npos);
}

static void test_html_to_text_removes_script_style() {
    const std::string html =
        "<html><head><title>Заголовок</title></head><body>"
        "<script>var x = 1;</script>"
        "<style>.cls { color: red; }</style>"
        "<p>Видимый текст</p>"
        "</body></html>";
    const std::string text = html_to_text(html);
    TEST_ASSERT(text.find("var x") == std::string::npos);
    TEST_ASSERT(text.find("color: red") == std::string::npos);
    TEST_ASSERT(text.find("Видимый текст") != std::string::npos);
}

static void test_html_to_text_decodes_entities() {
    const std::string html = "<p>AT&amp;T &lt; &gt; &quot;кавычки&quot;</p>";
    const std::string text = html_to_text(html);
    TEST_ASSERT(text.find("AT&T") != std::string::npos);
    TEST_ASSERT(text.find("<") != std::string::npos);
    TEST_ASSERT(text.find(">") != std::string::npos);
    TEST_ASSERT(text.find("\"кавычки\"") != std::string::npos);
}

static void test_html_to_text_removes_navigation_lines() {
    const std::string html =
        "<nav><ul><li>Главная</li><li>Новости</li><li>Контакты</li></ul></nav>"
        "<p>Здесь находится основной и достаточно длинный текст новости, "
        "который должен быть извлечён полностью.</p>";
    const std::string text = html_to_text(html);
    TEST_ASSERT(text.find("Главная") == std::string::npos);
    TEST_ASSERT(text.find("основной и достаточно длинный текст") != std::string::npos);
}

static void test_extract_from_description() {
    const std::string desc =
        "<p>Краткое описание новости с <a href=\"#\">ссылкой</a> и &quot;кавычками&quot;.</p>";
    const ExtractedArticle ex = extract_from_description(desc);
    TEST_ASSERT(ex.body.find("Краткое описание новости") != std::string::npos);
    TEST_ASSERT(ex.body.find("ссылк") != std::string::npos);
    TEST_ASSERT(ex.body.find("href") == std::string::npos);
}

static void test_extract_page_heuristic() {
    const std::string html =
        "<html><head><title>Тестовая страница</title></head><body>"
        "<h1>Заголовок новости</h1>"
        "<p>Первый абзац новости, довольно длинный и содержательный.</p>"
        "<p>Второй абзац с продолжением материала, тоже достаточно длинный, "
        "чтобы быть основным текстом статьи.</p>"
        "<footer>Копирайт</footer>"
        "</body></html>";
    const SourceExtract cfg;
    const ExtractedArticle ex = extract_page(html, cfg);
    TEST_ASSERT_EQUAL(ex.title, "Заголовок новости");
    TEST_ASSERT(ex.body.find("Первый абзац новости") != std::string::npos ||
                ex.body.find("Второй абзац") != std::string::npos);
    TEST_ASSERT(ex.body.find("Копирайт") == std::string::npos);
}

static void test_extract_page_markers() {
    const std::string html =
        "<html><body>"
        "<div id=\"menu\">Меню</div>"
        "<div class=\"article\">"
        "<span class=\"title\">Маркерный заголовок</span>"
        "<div class=\"content\">Текст по маркеру, длинный и важный для теста извлечения.</div>"
        "</div>"
        "</body></html>";
    SourceExtract cfg;
    cfg.title_marker = "<span class=\"title\">";
    cfg.body_marker = "<div class=\"content\">";
    const ExtractedArticle ex = extract_page(html, cfg);
    TEST_ASSERT_EQUAL(ex.title, "Маркерный заголовок");
    TEST_ASSERT(ex.body.find("Текст по маркеру") != std::string::npos);
}

static void test_extract_page_marker_only_body() {
    const std::string html = "<html><body><div class=\"content\">Только текст тела.</div></body></html>";
    SourceExtract cfg;
    cfg.body_marker = "<div class=\"content\">";
    const ExtractedArticle ex = extract_page(html, cfg);
    TEST_ASSERT(ex.body.find("Только текст тела") != std::string::npos);
}

static void test_extract_page_empty_html() {
    const ExtractedArticle ex = extract_page("", SourceExtract{});
    TEST_ASSERT_TRUE(ex.title.empty());
    TEST_ASSERT_TRUE(ex.body.empty());
}

REGISTER_TEST(test_html_to_text_strips_tags);
REGISTER_TEST(test_html_to_text_block_breaks);
REGISTER_TEST(test_html_to_text_removes_script_style);
REGISTER_TEST(test_html_to_text_decodes_entities);
REGISTER_TEST(test_html_to_text_removes_navigation_lines);
REGISTER_TEST(test_extract_from_description);
REGISTER_TEST(test_extract_page_heuristic);
REGISTER_TEST(test_extract_page_markers);
REGISTER_TEST(test_extract_page_marker_only_body);
REGISTER_TEST(test_extract_page_empty_html);

} // namespace
