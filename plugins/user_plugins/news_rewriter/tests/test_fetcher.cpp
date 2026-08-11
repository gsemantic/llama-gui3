#include "test_framework.h"

#include <string>

#include "fetcher.h"
#include "test_server.h"

using namespace news_rewriter;
using namespace news_rewriter_test;

namespace {

const char* kSampleRss =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<rss version=\"2.0\"><channel><title>Test</title>"
    "<item><title>Новость 1</title><link>http://x/1</link>"
    "<description>Текст первой новости</description></item>"
    "<item><title>Новость 2</title><link>http://x/2</link>"
    "<description>Текст второй новости</description></item>"
    "</channel></rss>";

const char* kSampleAtom =
    "<feed xmlns=\"http://www.w3.org/2005/Atom\">"
    "<entry><title>Atom 1</title><link href=\"http://x/atom1\" rel=\"alternate\"/>"
    "<summary>Сводка</summary></entry>"
    "</feed>";

} // namespace

static void test_fetcher_rss() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(200, kSampleRss, "application/rss+xml"));
    Fetcher f;
    TEST_ASSERT_TRUE(f.init());
    NetworkConfig cfg;
    const FetchResult res = f.fetch(srv.base_url() + "/feed", "rss", cfg);
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL(res.http_status, 200);
    TEST_ASSERT_EQUAL(res.items.size(), std::size_t(2));
    TEST_ASSERT_EQUAL(res.items[0].title, "Новость 1");
    TEST_ASSERT_EQUAL(res.items[0].link, "http://x/1");
    TEST_ASSERT_EQUAL(res.items[0].description, "Текст первой новости");
    TEST_ASSERT_TRUE(res.html.empty());
}

static void test_fetcher_atom() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(200, kSampleAtom, "application/atom+xml"));
    Fetcher f;
    TEST_ASSERT_TRUE(f.init());
    NetworkConfig cfg;
    const FetchResult res = f.fetch(srv.base_url() + "/atom", "atom", cfg);
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL(res.items.size(), std::size_t(1));
    TEST_ASSERT_EQUAL(res.items[0].title, "Atom 1");
    TEST_ASSERT_EQUAL(res.items[0].link, "http://x/atom1");
    TEST_ASSERT_EQUAL(res.items[0].description, "Сводка");
}

static void test_fetcher_page() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(200, "<html><body><h1>Привет</h1></body></html>", "text/html"));
    Fetcher f;
    TEST_ASSERT_TRUE(f.init());
    NetworkConfig cfg;
    const FetchResult res = f.fetch(srv.base_url() + "/page", "page", cfg);
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_TRUE(res.html.find("<html>") != std::string::npos);
    TEST_ASSERT_TRUE(res.items.empty());
}

static void test_fetcher_http_error() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(404, "not found"));
    Fetcher f;
    TEST_ASSERT_TRUE(f.init());
    NetworkConfig cfg;
    const FetchResult res = f.fetch(srv.base_url() + "/missing", "page", cfg);
    TEST_ASSERT_FALSE(res.ok);
    TEST_ASSERT_EQUAL(res.http_status, 404);
    TEST_ASSERT_TRUE(res.error.find("404") != std::string::npos);
}

static void test_fetcher_unknown_type() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(200, "x"));
    Fetcher f;
    TEST_ASSERT_TRUE(f.init());
    NetworkConfig cfg;
    const FetchResult res = f.fetch(srv.base_url(), "weird", cfg);
    TEST_ASSERT_FALSE(res.ok);
    TEST_ASSERT_TRUE(res.error.find("неизвестный тип") != std::string::npos);
}

static void test_fetcher_bad_xml() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(200, "this is not xml"));
    Fetcher f;
    TEST_ASSERT_TRUE(f.init());
    NetworkConfig cfg;
    const FetchResult res = f.fetch(srv.base_url() + "/bad", "rss", cfg);
    TEST_ASSERT_FALSE(res.ok);
    TEST_ASSERT_TRUE(res.error.find("XML") != std::string::npos);
    TEST_ASSERT_TRUE(res.permanent);   // HTML-страница без ленты — ошибка постоянная
}

// Тело новости в namespaced-теге <yandex:full-text> (как на epp.genproc.gov.ru).
static void test_fetcher_rss_full_text() {
    MiniHttpServer srv;
    const char* rss =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<rss version=\"2.0\" xmlns:yandex=\"http://news.yandex.ru\">"
        "<channel><title>T</title>"
        "<item><title>Новость</title><link>http://x/1</link>"
        "<pubDate>Sat, 01 Aug 2026 00:00:00 +0000</pubDate>"
        "<yandex:full-text>Полный текст новости из yandex:full-text</yandex:full-text>"
        "</item></channel></rss>";
    TEST_ASSERT_TRUE(srv.start(200, rss, "application/rss+xml"));
    Fetcher f;
    TEST_ASSERT_TRUE(f.init());
    NetworkConfig cfg;
    const FetchResult res = f.fetch(srv.base_url() + "/feed", "rss", cfg);
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL(res.items.size(), std::size_t(1));
    TEST_ASSERT_EQUAL(res.items[0].title, "Новость");
    TEST_ASSERT_EQUAL(res.items[0].description, "Полный текст новости из yandex:full-text");
}

// Если указана HTML-страница с <link rel="alternate" type="application/rss+xml">,
// Fetcher находит ленту и разбирает её (вместо ошибки "не XML").
static void test_fetcher_discovers_feed_link() {
    MiniHttpServer feed_srv;
    TEST_ASSERT_TRUE(feed_srv.start(200, kSampleRss, "application/rss+xml"));

    MiniHttpServer page_srv;
    const std::string html =
        "<html><head>"
        "<link rel=\"alternate\" type=\"application/rss+xml\" href=\"http://127.0.0.1:" +
        std::to_string(feed_srv.port()) + "/feed\">"
        "</head><body>Привет</body></html>";
    TEST_ASSERT_TRUE(page_srv.start(200, html, "text/html"));

    Fetcher f;
    TEST_ASSERT_TRUE(f.init());
    NetworkConfig cfg;
    const FetchResult res = f.fetch(page_srv.base_url() + "/", "rss", cfg);
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL(res.items.size(), std::size_t(2));
    TEST_ASSERT_EQUAL(res.items[0].title, "Новость 1");
    TEST_ASSERT_EQUAL(res.items[0].description, "Текст первой новости");
}

// HTML-страница с относительной ссылкой на ленту (тот же хост, путь /rss).
static void test_fetcher_discovers_relative_feed_link() {
    MiniHttpServer srv;
    const std::string html =
        "<html><head>"
        "<link rel=\"alternate\" type=\"application/rss+xml\" href=\"/rss\">"
        "</head><body>Привет</body></html>";
    TEST_ASSERT_TRUE(srv.start(200, html, "text/html"));
    srv.add_route("/rss", kSampleRss, "application/rss+xml");

    Fetcher f;
    TEST_ASSERT_TRUE(f.init());
    NetworkConfig cfg;
    const FetchResult res = f.fetch(srv.base_url() + "/", "rss", cfg);
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL(res.items.size(), std::size_t(2));
    TEST_ASSERT_EQUAL(res.items[0].title, "Новость 1");
}

// HTML-страница без <link> на ленту, но с лентой по соседнему пути "<страница>/rss/".
static void test_fetcher_sibling_feed_fallback() {
    MiniHttpServer srv;
    const std::string html = "<html><head><title>Прокуратура</title></head><body>Привет</body></html>";
    TEST_ASSERT_TRUE(srv.start(200, html, "text/html"));
    srv.add_route("/proc_33/rss/", kSampleRss, "application/rss+xml");

    Fetcher f;
    TEST_ASSERT_TRUE(f.init());
    NetworkConfig cfg;
    const FetchResult res = f.fetch(srv.base_url() + "/proc_33/", "rss", cfg);
    TEST_ASSERT_TRUE(res.ok);
    TEST_ASSERT_EQUAL(res.items.size(), std::size_t(2));
    TEST_ASSERT_EQUAL(res.items[0].title, "Новость 1");
}

REGISTER_TEST(test_fetcher_rss);
REGISTER_TEST(test_fetcher_atom);
REGISTER_TEST(test_fetcher_page);
REGISTER_TEST(test_fetcher_http_error);
REGISTER_TEST(test_fetcher_unknown_type);
REGISTER_TEST(test_fetcher_bad_xml);
REGISTER_TEST(test_fetcher_rss_full_text);
REGISTER_TEST(test_fetcher_discovers_feed_link);
REGISTER_TEST(test_fetcher_discovers_relative_feed_link);
REGISTER_TEST(test_fetcher_sibling_feed_fallback);
