#include "test_framework.h"

#include <string>

#include "common.h"

using namespace news_rewriter;

namespace {

// Опорные значения (UTC, секунды с эпохи):
//   2026-08-08 11:51:47 UTC = 1786189907
//   2026-08-08 08:51:47 UTC = 1786179107
//   2026-08-07 23:51:47 UTC = 1786146707

static void test_parse_feed_time_rfc822_utc() {
    const std::int64_t t = parse_feed_time("Sat, 08 Aug 2026 11:51:47 +0000");
    TEST_ASSERT_EQUAL(t, 1786189907);
}

static void test_parse_feed_time_rfc822_positive_zone() {
    const std::int64_t t = parse_feed_time("Sat, 08 Aug 2026 11:51:47 +0300");
    TEST_ASSERT_EQUAL(t, 1786179107);
}

static void test_parse_feed_time_rfc822_gmt() {
    const std::int64_t t = parse_feed_time("Fri, 07 Aug 2026 23:51:47 GMT");
    TEST_ASSERT_EQUAL(t, 1786146707);
}

static void test_parse_feed_time_iso_utc() {
    const std::int64_t t = parse_feed_time("2026-08-08T11:51:47Z");
    TEST_ASSERT_EQUAL(t, 1786189907);
}

static void test_parse_feed_time_iso_positive_zone() {
    const std::int64_t t = parse_feed_time("2026-08-08T14:51:47+03:00");
    TEST_ASSERT_EQUAL(t, 1786189907);
}

static void test_parse_feed_time_iso_zone_no_colon() {
    const std::int64_t t = parse_feed_time("2026-08-08T11:51:47+0000");
    TEST_ASSERT_EQUAL(t, 1786189907);
}

static void test_parse_feed_time_invalid() {
    TEST_ASSERT_EQUAL(parse_feed_time(""), 0);
    TEST_ASSERT_EQUAL(parse_feed_time("не дата"), 0);
    TEST_ASSERT_EQUAL(parse_feed_time("2026-08-08"), 0);       // без времени
    TEST_ASSERT_EQUAL(parse_feed_time("08/08/2026"), 0);
}

static void test_parse_feed_time_roundtrip_now() {
    // iso8601_now() должен разбираться обратно в близкое к текущему время.
    const std::int64_t t = parse_feed_time(iso8601_now());
    TEST_ASSERT(t > 0);
}

REGISTER_TEST(test_parse_feed_time_rfc822_utc);
REGISTER_TEST(test_parse_feed_time_rfc822_positive_zone);
REGISTER_TEST(test_parse_feed_time_rfc822_gmt);
REGISTER_TEST(test_parse_feed_time_iso_utc);
REGISTER_TEST(test_parse_feed_time_iso_positive_zone);
REGISTER_TEST(test_parse_feed_time_iso_zone_no_colon);
REGISTER_TEST(test_parse_feed_time_invalid);
REGISTER_TEST(test_parse_feed_time_roundtrip_now);

} // namespace
