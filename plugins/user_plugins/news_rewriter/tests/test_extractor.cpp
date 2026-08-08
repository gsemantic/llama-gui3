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

// Меню, сайдбар и подвал исключаются; в тело попадает только статья.
static void test_extract_body_skips_nav_footer() {
    const std::string html =
        "<html><head><title>Новая разработка в отрасли</title></head><body>"
        "<nav><ul><li>Главная</li><li>Политика</li><li>Экономика</li><li>Спорт</li></ul></nav>"
        "<div class=\"content\">"
        "<h2>Новая разработка в отрасли</h2>"
        "<p>Первый абзац основной статьи, в котором излагается суть события достаточно подробно.</p>"
        "<p>Второй абзац продолжает материал и содержит важные детали и подробности.</p>"
        "<p>Третий абзац завершает текст, подводя итог и делая выводы о произошедшем.</p>"
        "</div>"
        "<aside>Реклама и прочая боковая информация, не относящаяся к статье.</aside>"
        "<footer>© 2024 Новости. Все права защищены.</footer>"
        "</body></html>";
    const ExtractedArticle ex = extract_page(html, SourceExtract{});
    TEST_ASSERT_EQUAL(ex.title, "Новая разработка в отрасли");
    TEST_ASSERT(ex.body.find("Главная") == std::string::npos);
    TEST_ASSERT(ex.body.find("Политика") == std::string::npos);
    TEST_ASSERT(ex.body.find("Новая разработка в отрасли") == std::string::npos);
    TEST_ASSERT(ex.body.find("Первый абзац основной статьи") != std::string::npos);
    TEST_ASSERT(ex.body.find("Второй абзац") != std::string::npos);
    TEST_ASSERT(ex.body.find("Третий абзац") != std::string::npos);
    TEST_ASSERT(ex.body.find("Реклама и прочая") == std::string::npos);
    TEST_ASSERT(ex.body.find("Все права защищены") == std::string::npos);
}

// Статья, разбитая на несколько абзацев, извлекается целиком (связный набор),
// а не как единственный самый длинный блок.
static void test_extract_body_merges_paragraphs() {
    const std::string html =
        "<html><body>"
        "<p>Первый абзац статьи, достаточно длинный и содержательный, чтобы быть частью основного текста.</p>"
        "<p>Второй абзац статьи, тоже длинный и содержательный, продолжает материал дальше.</p>"
        "<p>Третий абзац статьи, не менее длинный, завершает основное содержание.</p>"
        "</body></html>";
    const ExtractedArticle ex = extract_page(html, SourceExtract{});
    TEST_ASSERT(ex.body.find("Первый абзац статьи") != std::string::npos);
    TEST_ASSERT(ex.body.find("Второй абзац статьи") != std::string::npos);
    TEST_ASSERT(ex.body.find("Третий абзац статьи") != std::string::npos);
}

// Блок с низкой плотностью текста (счётчики, символы) не считается статьёй.
static void test_extract_body_prefers_dense_text() {
    const std::string html =
        "<html><body>"
        "<p>12:45 2024 © # %% 13</p>"
        "<p>Основной текст новости, который действительно нужно извлечь, содержит связное предложение.</p>"
        "</body></html>";
    const ExtractedArticle ex = extract_page(html, SourceExtract{});
    TEST_ASSERT(ex.body.find("Основной текст новости") != std::string::npos);
    TEST_ASSERT(ex.body.find("12:45") == std::string::npos);
}

// Статья целиком в одном блоке без отдельных абзацев извлекается полностью.
static void test_extract_body_single_block() {
    const std::string html =
        "<html><body><div class=\"content\">"
        "Здесь весь текст статьи умещается в один большой блок без отдельных абзацев, "
        "и extractor должен вернуть его целиком, не потеряв ни одного слова."
        "</div></body></html>";
    const ExtractedArticle ex = extract_page(html, SourceExtract{});
    TEST_ASSERT(ex.body.find("весь текст статьи") != std::string::npos);
    TEST_ASSERT(ex.body.find("не потеряв ни одного слова") != std::string::npos);
}

// Заголовок статьи (h1) не дублируется в теле.
static void test_extract_body_excludes_title() {
    const std::string html =
        "<html><head><title>Заголовок статьи</title></head><body>"
        "<h1>Заголовок статьи</h1>"
        "<p>Единственный абзац основного текста, который должен быть извлечён как тело.</p>"
        "</body></html>";
    const ExtractedArticle ex = extract_page(html, SourceExtract{});
    TEST_ASSERT_EQUAL(ex.title, "Заголовок статьи");
    TEST_ASSERT(ex.body.find("Единственный абзац") != std::string::npos);
    TEST_ASSERT(ex.body.find("Заголовок статьи") == std::string::npos);
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
REGISTER_TEST(test_extract_body_skips_nav_footer);
REGISTER_TEST(test_extract_body_merges_paragraphs);
REGISTER_TEST(test_extract_body_prefers_dense_text);
REGISTER_TEST(test_extract_body_single_block);
REGISTER_TEST(test_extract_body_excludes_title);

} // namespace
