#include "test_framework.h"

#include <string>

#include "rewriter.h"

using namespace news_rewriter;

static void test_seo_title_truncated_to_60() {
    std::string long_title(80, 'y');
    const std::string json =
        "{\"focus_keyword\":\"ai\",\"meta_description\":\"d\",\"seo_title\":\"" +
        long_title + "\"}";
    const SeoResult r = parse_seo_response(json);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_TRUE(r.seo_title.size() <= 63);          // 60 + 3-байтный '…'
    TEST_ASSERT_EQUAL(r.seo_title.substr(0, 60), std::string(60, 'y'));
    TEST_ASSERT_EQUAL(r.seo_title.substr(60), std::string("…"));
}

static void test_seo_description_truncated_to_160() {
    std::string long_desc(200, 'x');
    const std::string json =
        "{\"focus_keyword\":\"ai\",\"meta_description\":\"" + long_desc +
        "\",\"seo_title\":\"t\"}";
    const SeoResult r = parse_seo_response(json);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_TRUE(r.meta_description.size() <= 163);  // 160 + 3-байтный '…'
    TEST_ASSERT_EQUAL(r.meta_description.substr(0, 160), std::string(160, 'x'));
    TEST_ASSERT_EQUAL(r.meta_description.substr(160), std::string("…"));
}

static void test_seo_keyword_lowercased_and_limited() {
    const std::string json =
        "{\"focus_keyword\":\"ИСКУССТВЕННЫЙ ИНТЕЛЛЕКТ НОВОСТИ ДЛЯ САЙТА EXTRA\","
        "\"meta_description\":\"d\",\"seo_title\":\"t\"}";
    const SeoResult r = parse_seo_response(json);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL(r.focus_keyword, "искусственный интеллект новости для");
    // slug выводится из ключевой фразы
    TEST_ASSERT_EQUAL(r.seo_slug, "iskusstvennyi-intellekt-novosti-dlya");
}

static void test_seo_short_values_untouched() {
    const std::string json =
        "{\"focus_keyword\":\"ai news\",\"meta_description\":\"коротко\",\"seo_title\":\"Заг\"}";
    const SeoResult r = parse_seo_response(json);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL(r.seo_title, "Заг");
    TEST_ASSERT_EQUAL(r.meta_description, "коротко");
    TEST_ASSERT_EQUAL(r.focus_keyword, "ai news");
    TEST_ASSERT_EQUAL(r.seo_slug, "ai-news");
}

REGISTER_TEST(test_seo_title_truncated_to_60);
REGISTER_TEST(test_seo_description_truncated_to_160);
REGISTER_TEST(test_seo_keyword_lowercased_and_limited);
REGISTER_TEST(test_seo_short_values_untouched);
