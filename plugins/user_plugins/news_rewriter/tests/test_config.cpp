#include "test_framework.h"

#include <string>

#include "config.h"

using namespace news_rewriter;

namespace {

static void test_config_roundtrip_new_fields() {
    Config cfg = default_config();
    cfg.rewrite.max_words = 250;
    cfg.sink.output_dir = "/tmp/my_news";
    cfg.max_items_per_source = 10;
    cfg.max_age_hours = 48;
    cfg.max_retries = 5;
    cfg.schedule_minutes = 30;
    cfg.network.timeout_seconds = 42;

    const Config back = config_from_json(config_to_json(cfg));
    TEST_ASSERT_EQUAL(back.rewrite.max_words, 250);
    TEST_ASSERT_EQUAL(back.sink.output_dir, "/tmp/my_news");
    TEST_ASSERT_EQUAL(back.max_items_per_source, 10);
    TEST_ASSERT_EQUAL(back.max_age_hours, 48);
    TEST_ASSERT_EQUAL(back.max_retries, 5);
    TEST_ASSERT_EQUAL(back.schedule_minutes, 30);
    TEST_ASSERT_EQUAL(back.network.timeout_seconds, 42);
}

static void test_config_from_json_defaults() {
    const Config cfg = config_from_json(Json::object());
    TEST_ASSERT_EQUAL(cfg.max_retries, 3);
    TEST_ASSERT_EQUAL(cfg.max_age_hours, 0);
    TEST_ASSERT_EQUAL(cfg.max_items_per_source, 0);
    TEST_ASSERT_EQUAL(cfg.rewrite.max_words, 0);
    TEST_ASSERT_EQUAL(cfg.sink.output_dir, "");
    TEST_ASSERT_EQUAL(cfg.sources.size(), std::size_t(1));
}

static void test_config_source_roundtrip() {
    Config cfg = default_config();
    cfg.sources.push_back(SourceConfig{"https://c.example/atom", "atom",
                                       SourceExtract{"<h1>", "<p>"}, false});
    const Config back = config_from_json(config_to_json(cfg));
    TEST_ASSERT_EQUAL(back.sources.size(), std::size_t(2));
    TEST_ASSERT_EQUAL(back.sources[1].url, "https://c.example/atom");
    TEST_ASSERT_EQUAL(back.sources[1].type, "atom");
    TEST_ASSERT_FALSE(back.sources[1].enabled);
    TEST_ASSERT_EQUAL(back.sources[1].extract.title_marker, "<h1>");
    TEST_ASSERT_EQUAL(back.sources[1].extract.body_marker, "<p>");
}

static void test_config_network_roundtrip() {
    Config cfg = default_config();
    cfg.network.timeout_seconds = 7;
    cfg.network.user_agent = "my-agent/2.0";
    cfg.network.proxy = "http://proxy.local:8080";
    cfg.network.extra_headers = "Authorization: Bearer abc\nX-Custom: 1";

    const Config back = config_from_json(config_to_json(cfg));
    TEST_ASSERT_EQUAL(back.network.timeout_seconds, 7);
    TEST_ASSERT_EQUAL(back.network.user_agent, "my-agent/2.0");
    TEST_ASSERT_EQUAL(back.network.proxy, "http://proxy.local:8080");
    TEST_ASSERT_EQUAL(back.network.extra_headers, "Authorization: Bearer abc\nX-Custom: 1");
}

REGISTER_TEST(test_config_roundtrip_new_fields);
REGISTER_TEST(test_config_from_json_defaults);
REGISTER_TEST(test_config_source_roundtrip);
REGISTER_TEST(test_config_network_roundtrip);

} // namespace
