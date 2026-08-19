#include "test_framework.h"

#include <string>

#include "common.h"
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
    const ExtractedArticle ex = extract_page(html, "", cfg);
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
    const ExtractedArticle ex = extract_page(html, "", cfg);
    TEST_ASSERT_EQUAL(ex.title, "Маркерный заголовок");
    TEST_ASSERT(ex.body.find("Текст по маркеру") != std::string::npos);
}

static void test_extract_page_marker_only_body() {
    const std::string html = "<html><body><div class=\"content\">Только текст тела.</div></body></html>";
    SourceExtract cfg;
    cfg.body_marker = "<div class=\"content\">";
    const ExtractedArticle ex = extract_page(html, "", cfg);
    TEST_ASSERT(ex.body.find("Только текст тела") != std::string::npos);
}

static void test_extract_page_empty_html() {
    const ExtractedArticle ex = extract_page("", "", SourceExtract{});
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
    const ExtractedArticle ex = extract_page(html, "", SourceExtract{});
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
    const ExtractedArticle ex = extract_page(html, "", SourceExtract{});
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
    const ExtractedArticle ex = extract_page(html, "", SourceExtract{});
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
    const ExtractedArticle ex = extract_page(html, "", SourceExtract{});
    TEST_ASSERT(ex.body.find("весь текст статьи") != std::string::npos);
    TEST_ASSERT(ex.body.find("не потеряв ни одного слова") != std::string::npos);
}

// Служебный баннер cookies («Обновление правил использования cookies») не должен
// подменять собой текст новости: он лежит в div с «шумным» классом и исключается.
static void test_extract_body_excludes_cookie_banner() {
    const std::string html =
        "<html><head><title>Важная новость дня</title></head><body>"
        "<div class=\"article\">"
        "<h1>Важная новость дня</h1>"
        "<p>Первый абзац основной новости, в котором рассказывается о произошедшем событии подробно.</p>"
        "<p>Второй абзац продолжает материал и раскрывает важные детали и обстоятельства.</p>"
        "<p>Третий абзац завершает текст и подводит итог произошедшему.</p>"
        "</div>"
        "<div class=\"cookie-consent\" id=\"cookieBanner\">"
        "Обновление правил использования cookies на сайте. Мы используем файлы cookie, "
        "чтобы улучшить работу сайта. Продолжая, вы соглашаетесь с политикой конфиденциальности."
        "</div>"
        "</body></html>";
    const ExtractedArticle ex = extract_page(html, "", SourceExtract{});
    TEST_ASSERT_EQUAL(ex.title, "Важная новость дня");
    TEST_ASSERT(ex.body.find("Первый абзац основной новости") != std::string::npos);
    TEST_ASSERT(ex.body.find("Второй абзац") != std::string::npos);
    TEST_ASSERT(ex.body.find("Третий абзац") != std::string::npos);
    TEST_ASSERT(ex.body.find("Обновление правил использования cookies") == std::string::npos);
    TEST_ASSERT(ex.body.find("соглашаетесь с политикой") == std::string::npos);
}

// Заголовок статьи (h1) не дублируется в теле.
static void test_extract_body_excludes_title() {
    const std::string html =
        "<html><head><title>Заголовок статьи</title></head><body>"
        "<h1>Заголовок статьи</h1>"
        "<p>Единственный абзац основного текста, который должен быть извлечён как тело.</p>"
        "</body></html>";
    const ExtractedArticle ex = extract_page(html, "", SourceExtract{});
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
// Заголовок статьи берётся из ТОГО ЖЕ региона, что и тело: если в начале
// страницы (в <header>) есть чужой <h1>, он не должен «разъезжаться» с телом.
// Регрессия: раньше extract_title брал первый <h1> страницы, и в режиме
// разведки показывался заголовок, не соответствующий телу новости.
static void test_extract_title_matches_body_region() {
    std::string filler;
    for (int i = 0; i < 200; ++i) {
        filler += "Пункт " + std::to_string(i) + " ::: %%% 12 34\n";
    }
    const std::string html =
        "<html><body>"
        "<header><h1>Совсем не та новость про случайную тему</h1></header>"
        + filler +
        "<div class=\"content\">"
        "<h1>Важная новость дня о событии</h1>"
        "<p>Первый абзац основной новости, в котором рассказывается о произошедшем событии подробно и обстоятельно.</p>"
        "<p>Второй абзац продолжает материал и раскрывает важные детали и обстоятельства происшествия.</p>"
        "<p>Третий абзац завершает текст и подводит итог произошедшему событию в городе.</p>"
        "</div>"
        "</body></html>";
    const ExtractedArticle ex = extract_page(html, "", SourceExtract{});
    TEST_ASSERT_EQUAL(ex.title, "Важная новость дня о событии");
    TEST_ASSERT(ex.body.find("Первый абзац основной новости") != std::string::npos);
    TEST_ASSERT(ex.body.find("Совсем не та новость") == std::string::npos);
}

REGISTER_TEST(test_extract_body_skips_nav_footer);
REGISTER_TEST(test_extract_body_merges_paragraphs);
REGISTER_TEST(test_extract_body_prefers_dense_text);
REGISTER_TEST(test_extract_body_single_block);
REGISTER_TEST(test_extract_body_excludes_cookie_banner);
REGISTER_TEST(test_extract_body_excludes_title);
REGISTER_TEST(test_extract_title_matches_body_region);

// Блок-каталог новостей (news-catalog) внутри страницы статьи не должен
// попадать в тело как дайджест: extract_page вырезает ленты и берёт только
// саму статью. Регрессия: раньше плотный список новостей «побеждал» статью.
static void test_extract_page_excludes_news_catalog() {
    std::string catalog;
    for (int i = 0; i < 5; ++i) {
        catalog += "<div class=\"news__content\"><a href=\"/n" +
                   std::to_string(i) + "\">Каталожная новость номер " +
                   std::to_string(i) + "</a>"
                   "<p>Короткий анонс каталожной новости номер " +
                   std::to_string(i) + " про совсем другое событие.</p></div>";
    }
    const std::string html =
        "<html><body>"
        "<div class=\"news-catalog\">" + catalog + "</div>"
        "<div class=\"text\">"
        "<h1>Единственная настоящая статья про событие дня</h1>"
        "<p>Первый абзац единственной настоящей статьи, в котором рассказывается о произошедшем событии подробно и обстоятельно.</p>"
        "<p>Второй абзац настоящей статьи продолжает материал и раскрывает важные детали происшествия.</p>"
        "<p>Третий абзац настоящей статьи завершает текст и подводит итог произошедшему.</p>"
        "</div>"
        "</body></html>";
    const ExtractedArticle ex = extract_page(html, "", SourceExtract{});
    TEST_ASSERT_EQUAL(ex.title, "Единственная настоящая статья про событие дня");
    TEST_ASSERT(ex.body.find("Первый абзац единственной настоящей статьи") !=
                std::string::npos);
    TEST_ASSERT(ex.body.find("Каталожная новость") == std::string::npos);
}

REGISTER_TEST(test_extract_page_excludes_news_catalog);

static void test_extract_page_items_listing_splits_articles() {
    const std::string html =
        "<html><body>"
        "<article><h2><a href=\"/news/a1\">Первая новость</a></h2>"
        "<img src=\"http://x/1.jpg\"><p>Текст первой новости про событие.</p></article>"
        "<article><h2><a href=\"/news/a2\">Вторая новость</a></h2>"
        "<img src=\"http://x/2.jpg\"><p>Текст второй новости про другое.</p></article>"
        "</body></html>";
    const std::vector<ExtractedArticle> items =
        extract_page_items(html, "http://example.com/category", SourceExtract{});
    TEST_ASSERT_EQUAL(items.size(), 2u);
    TEST_ASSERT_EQUAL(items[0].url, "http://example.com/news/a1");
    TEST_ASSERT_EQUAL(items[0].title, "Первая новость");
    TEST_ASSERT_EQUAL(items[0].image, "http://x/1.jpg");
    TEST_ASSERT(items[0].body.find("Текст первой новости") != std::string::npos);
    TEST_ASSERT_EQUAL(items[1].url, "http://example.com/news/a2");
    TEST_ASSERT_EQUAL(items[1].image, "http://x/2.jpg");
}

static void test_extract_page_items_single_article_stays_one() {
    const std::string html =
        "<html><head><title>Одна статья</title></head><body>"
        "<h1>Одна статья</h1>"
        "<p>Единственный абзац основного текста статьи.</p>"
        "</body></html>";
    const std::vector<ExtractedArticle> items =
        extract_page_items(html, "http://example.com/post", SourceExtract{});
    TEST_ASSERT_EQUAL(items.size(), 1u);
    TEST_ASSERT_EQUAL(items[0].title, "Одна статья");
    TEST_ASSERT(items[0].body.find("Единственный абзац") != std::string::npos);
}

// Главная/категория, где новости лежат внутри news-catalog: страница должна
// распознаваться как СПИСОК статей, а не как одна статья (иначе вместо новостей
// тянется логотип/шапка сайта). Регрессия после добавления вырезки каталогов:
// она не должна применяться при поиске списка статей.
static void test_extract_page_items_homepage_catalog_is_list() {
    const std::string html =
        "<html><body>"
        "<div class=\"news-catalog\">"
        "<div class=\"news-catalog__item\"><a href=\"/novosti/a1\">Первая важная новость дня</a><img src=\"http://x/1.jpg\"></div>"
        "<div class=\"news-catalog__item\"><a href=\"/novosti/a2\">Вторая срочная новость города</a><img src=\"http://x/2.jpg\"></div>"
        "<div class=\"news-catalog__item\"><a href=\"/novosti/a3\">Третья свежая новость региона</a><img src=\"http://x/3.jpg\"></div>"
        "</div>"
        "</body></html>";
    const std::vector<ExtractedArticle> items =
        extract_page_items(html, "http://zebra-tv.ru", SourceExtract{});
    TEST_ASSERT(items.size() >= 2);
    for (const auto& it : items) {
        TEST_ASSERT(!it.url.empty());
        TEST_ASSERT(!it.title.empty());
    }
}

REGISTER_TEST(test_extract_page_items_listing_splits_articles);
REGISTER_TEST(test_extract_page_items_homepage_catalog_is_list);
REGISTER_TEST(test_extract_page_items_single_article_stays_one);

static void test_extract_page_items_skips_sidebar_widget() {
    const std::string html =
        "<html><body>"
        "<article><h2><a href=\"/news/a1\">Свежая новость</a></h2>"
        "<img src=\"http://x/1.jpg\"><p>Текст свежей новости.</p></article>"
        "<article><h2><a href=\"/news/a2\">Ещё одна свежая новость</a></h2>"
        "<img src=\"http://x/2.jpg\"><p>Текст второй новости.</p></article>"
        "<aside class=\"sidebar\"><h2>Постоянная правая информация</h2>"
        "<article><a href=\"/widget/w1\">Давно размещённый виджет</a>"
        "<p>Старая справочная плашка.</p></article></aside>"
        "</body></html>";
    const std::vector<ExtractedArticle> items =
        extract_page_items(html, "http://example.com/cat", SourceExtract{});
    // Реальных новостей две; «постоянная правая информация» исключена сайдбаром.
    TEST_ASSERT_EQUAL(items.size(), 2u);
    TEST_ASSERT_EQUAL(items[0].url, "http://example.com/news/a1");
    TEST_ASSERT_EQUAL(items[1].url, "http://example.com/news/a2");
    for (const auto& it : items) {
        TEST_ASSERT(it.title.find("Постоянная правая") == std::string::npos);
        TEST_ASSERT(it.url.find("/widget/") == std::string::npos);
    }
}

REGISTER_TEST(test_extract_page_items_skips_sidebar_widget);

// <time datetime="..."> на листинге разбирается в published_at (Unix UTC).
static void test_extract_page_items_parses_time_datetime() {
    const std::string html =
        "<html><body>"
        "<article><time datetime=\"2026-08-13T10:00:00Z\"></time>"
        "<h2><a href=\"/news/a1\">Первая новость</a></h2>"
        "<p>Текст первой новости.</p></article>"
        "<article><time datetime=\"2026-08-12T09:00:00+03:00\"></time>"
        "<h2><a href=\"/news/a2\">Вторая новость</a></h2>"
        "<p>Текст второй новости.</p></article>"
        "</body></html>";
    const std::vector<ExtractedArticle> items =
        extract_page_items(html, "http://example.com/cat", SourceExtract{});
    TEST_ASSERT_EQUAL(items.size(), 2u);
    TEST_ASSERT_EQUAL(items[0].published_at,
                      parse_feed_time("2026-08-13T10:00:00Z"));
    TEST_ASSERT_EQUAL(items[1].published_at,
                      parse_feed_time("2026-08-12T09:00:00+03:00"));
}

// Текстовые даты (рус «13 августа 2026» и англ «Aug 11, 2026») тоже
// разбираются в published_at.
static void test_extract_page_items_parses_textual_date() {
    const std::string html =
        "<html><body>"
        "<article><span class=\"date\">13 августа 2026</span>"
        "<h2><a href=\"/news/a1\">Первая новость</a></h2>"
        "<p>Текст первой новости.</p></article>"
        "<article><span class=\"date\">Aug 11, 2026</span>"
        "<h2><a href=\"/news/a2\">Вторая новость</a></h2>"
        "<p>Текст второй новости.</p></article>"
        "</body></html>";
    const std::vector<ExtractedArticle> items =
        extract_page_items(html, "http://example.com/cat", SourceExtract{});
    TEST_ASSERT_EQUAL(items.size(), 2u);
    // «13 августа 2026» → 2026-08-13 (полночь UTC).
    TEST_ASSERT_EQUAL(items[0].published_at,
                      parse_feed_time("2026-08-13T00:00:00"));
    // «Aug 11, 2026» → 2026-08-11 (полночь UTC).
    TEST_ASSERT_EQUAL(items[1].published_at,
                      parse_feed_time("2026-08-11T00:00:00"));
}

REGISTER_TEST(test_extract_page_items_parses_time_datetime);
REGISTER_TEST(test_extract_page_items_parses_textual_date);

// Картинка статьи не должна быть логотипом сайта или og:image-превью с
// наложением заголовка — берём первое содержательное фото из тела.
static void test_first_content_image_skips_logo_and_og() {
    const std::string html =
        "<html><head><meta property='og:image' content='https://x/og-images/preview.jpg'>"
        "</head><body>"
        "<img src='/bitrix/logo.svg'>"
        "<img src='/upload/iblock/7a4/foto_sezd.jpg'>"
        "<img src='/upload/medialibrary/9b4/photo_1.jpg'>"
        "</body></html>";
    TEST_ASSERT_EQUAL(first_content_image(html), "/upload/iblock/7a4/foto_sezd.jpg");
}
REGISTER_TEST(test_first_content_image_skips_logo_and_og);

// Регрессия: обложка статьи (Дзен: content--zen-image-cover, hero/постер) с
// наложением текста поверх изображения не должна попадать в главное фото —
// берём первое настоящее фото из тела (article-image-item). Обложка и фото
// лежат на одном CDN, поэтому отличаются только class-ом тега <img>.
static void test_first_content_image_skips_cover_on_dzen() {
    const std::string html =
        "<article>"
        "<img class='content--zen-image-cover__image-2x' "
        "src='https://avatars.dzeninfra.ru/get-zen_doc/x/smart_crop_516x290'>"
        "<h1>Заголовок</h1>"
        "<p>Текст новости довольно длинный и содержательный абзац статьи.</p>"
        "<img class='content--article-image-item__image-3_' itemprop='image' "
        "src='https://avatars.dzeninfra.ru/get-zen_doc/x/scale_1200'>"
        "</article>";
    TEST_ASSERT_EQUAL(first_content_image(html),
                      "https://avatars.dzeninfra.ru/get-zen_doc/x/scale_1200");
}
REGISTER_TEST(test_first_content_image_skips_cover_on_dzen);

// Регрессия: hero/постер-картинка (class содержит «hero») пропускается, даже
// если это единственная картинка перед фото статьи.
static void test_first_content_image_skips_hero_class() {
    const std::string html =
        "<img class='hero-image' src='https://cdn.example.com/hero.jpg'>"
        "<p>Основной текст новости достаточно длинный для тела статьи.</p>"
        "<img class='content-image' src='https://cdn.example.com/real.jpg'>";
    TEST_ASSERT_EQUAL(first_content_image(html), "https://cdn.example.com/real.jpg");
}
REGISTER_TEST(test_first_content_image_skips_hero_class);

// Регрессия: иллюстрация глубоко в теле статьи (не в начале) должна
// находиться, если перед ней нет других подходящих картинок.
static void test_extract_page_finds_deep_body_image() {
    const std::string html =
        "<html><head><title>Статья о событии</title></head><body>"
        "<div class='article'>"
        "<h1>Заголовок статьи о важном событии дня</h1>"
        "<p>Первый абзац статьи с подробным описанием события и его предыстории, "
        "достаточно длинный чтобы быть основным текстом для эвристики плотности.</p>"
        "<p>Второй абзац продолжает материал и содержит важные детали происходящего "
        "и комментарии участников, а также анализ ситуации в целом и прогнозы.</p>"
        "<p>Третий абзац завершает повествование и подводит итог произошедшего "
        "события, делая выводы о его значении и последствиях для отрасли.</p>"
        "<figure class='article-image'>"
        "<img src='https://cdn.example.com/deep-illustration.jpg' alt='иллюстрация'></figure>"
        "</div>"
        "</body></html>";
    const ExtractedArticle ex = extract_page(html, "http://example.com/", SourceExtract{});
    TEST_ASSERT(ex.image.find("deep-illustration.jpg") != std::string::npos);
}
REGISTER_TEST(test_extract_page_finds_deep_body_image);

// Регрессия: обложка с BEM-классом через подчёркивание (block__element--mod),
// напр. "...image-cover__image", должна пропускаться, а главное фото —
// браться из глубины текста. Раньше token_contains считал '_' частью токена,
// поэтому "cover" за '_' не распознавался и возвращалась обложка вместо
// настоящей иллюстрации статьи.
static void test_first_content_image_skips_cover_with_underscore() {
    const std::string html =
        "<article>"
        "<img class='media__cover-image hero_cover' "
        "src='https://cdn.example.com/cover.jpg'>"
        "<h1>Заголовок</h1>"
        "<p>Текст новости довольно длинный и содержательный абзац статьи.</p>"
        "<img class='article__image content-image' "
        "src='https://cdn.example.com/real-photo.jpg'>"
        "</article>";
    TEST_ASSERT_EQUAL(first_content_image(html),
                      "https://cdn.example.com/real-photo.jpg");
}
REGISTER_TEST(test_first_content_image_skips_cover_with_underscore);

// Регрессия: составной class вроде «content__main_with-aside» содержит слово
// «aside», но это главная колонка с новостями, а не сайдбар. Его нельзя
// вырезать — иначе вся лента теряется и страница обрабатывается как одна статья.
static void test_strip_keeps_main_content_with_aside() {
    const std::string html =
        "<html><body>"
        "<div class='content__main content__main_with-aside'>"
        "<a href='/news/a1'>Первая новость про кота во Владимире</a>"
        "<a href='/news/a2'>Вторая новость про пса во Владимире</a>"
        "</div>"
        "<div class='sidebar'><a href='/news/x'>Коротко</a></div>"
        "</body></html>";
    const std::vector<ExtractedArticle> items =
        extract_page_items(html, "http://example.com/", SourceExtract{});
    TEST_ASSERT(items.size() >= 2u);
}
REGISTER_TEST(test_strip_keeps_main_content_with_aside);

} // namespace
