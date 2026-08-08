#include "test_framework.h"

#include <string>

#include "sink.h"
#include "storage.h"
#include "test_server.h"

using namespace news_rewriter;
using namespace news_rewriter_test;

namespace {

Article make_article() {
    Article a;
    a.id = "abc123";
    a.url = "https://example.com/news/1";
    a.source = "example.com";
    a.fetched_at = "2026-08-08T12:00:00Z";
    a.title_original = "Заголовок";
    a.body_original = "Текст новости.";
    a.title_rewritten = "Новый заголовок";
    a.body_rewritten = "Новый текст.";
    a.language = "ru";
    a.content_hash = "hash";
    return a;
}

SinkConfig make_config(const std::string& url, const std::string& api_key = "") {
    SinkConfig cfg;
    cfg.type = "http";
    cfg.params = Json::object();
    cfg.params["url"] = url;
    if (!api_key.empty()) cfg.params["api_key"] = api_key;
    return cfg;
}

} // namespace

static void test_http_sink_posts_json() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(200, "ok"));

    Storage storage;
    const auto sink = make_http_sink(
        make_config(srv.base_url() + "/ingest", "secret-key"), storage, nullptr);

    TEST_ASSERT_TRUE(sink != nullptr);
    TEST_ASSERT_EQUAL(std::string(sink->name()), "http");
    TEST_ASSERT_TRUE(sink->write(make_article()));

    const std::string req = srv.last_request();
    TEST_ASSERT(req.find("POST /ingest") != std::string::npos);
    TEST_ASSERT(req.find("Content-Type: application/json") != std::string::npos);
    TEST_ASSERT(req.find("Authorization: Bearer secret-key") != std::string::npos);
    TEST_ASSERT(req.find("title_original") != std::string::npos);   // JSON-тело
}

static void test_http_sink_server_error() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(500, "boom"));

    Storage storage;
    const auto sink = make_http_sink(
        make_config(srv.base_url() + "/fail"), storage, nullptr);
    TEST_ASSERT_FALSE(sink->write(make_article()));   // 5xx → неуспех
}

static void test_http_sink_no_url() {
    Storage storage;
    const auto sink = make_http_sink(make_config(""), storage, nullptr);
    TEST_ASSERT_FALSE(sink->write(make_article()));
}

static void test_http_sink_retries_on_failure() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(503, "try later"));

    SinkConfig cfg = make_config(srv.base_url() + "/retry");
    cfg.params["max_retries"] = 2;
    cfg.params["retry_delay_ms"] = 0;

    Storage storage;
    const auto sink = make_http_sink(cfg, storage, nullptr);
    TEST_ASSERT_FALSE(sink->write(make_article()));   // после всех ретраев — неуспех
    TEST_ASSERT_EQUAL(srv.request_count(), 3);        // 1 + 2 ретрая
}

REGISTER_TEST(test_http_sink_posts_json);
REGISTER_TEST(test_http_sink_server_error);
REGISTER_TEST(test_http_sink_no_url);
REGISTER_TEST(test_http_sink_retries_on_failure);
