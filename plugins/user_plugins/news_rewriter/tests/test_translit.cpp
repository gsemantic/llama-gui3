#include "test_framework.h"

#include <string>

#include "translit.h"

using namespace news_rewriter;

static void test_translit_cyrillic() {
    TEST_ASSERT_EQUAL(transliterate_to_latin("Привет"), "Privet");
    TEST_ASSERT_EQUAL(transliterate_to_latin("ён"), "en");
    // мягкий/твёрдый знак опускаются; щ/ч/ш/ж -> sch/ch/sh/zh
    TEST_ASSERT_EQUAL(transliterate_to_latin("Съешь жёлтый щенок"), "Sesh zheltyi schenok");
}

static void test_translit_latin_passthrough() {
    TEST_ASSERT_EQUAL(transliterate_to_latin("Hello World"), "Hello World");
}

static void test_make_slug_basic() {
    TEST_ASSERT_EQUAL(make_slug("Искусственный интеллект"),
                      "iskusstvennyi-intellekt");
    TEST_ASSERT_EQUAL(make_slug("News About AI"), "news-about-ai");
}

static void test_make_slug_stopwords() {
    // "о" — стоп-слово, вырезается
    TEST_ASSERT_EQUAL(make_slug("Новости о мире"), "novosti-mire");
}

static void test_make_slug_collapse_and_trim() {
    TEST_ASSERT_EQUAL(make_slug("  ИИ !!  "), "ii");
    // "a" — английский стоп-слово, вырезается; остаётся "b"
    TEST_ASSERT_EQUAL(make_slug("a---b"), "b");
    TEST_ASSERT_EQUAL(make_slug("foo___bar"), "foo-bar");
}

REGISTER_TEST(test_translit_cyrillic);
REGISTER_TEST(test_translit_latin_passthrough);
REGISTER_TEST(test_make_slug_basic);
REGISTER_TEST(test_make_slug_stopwords);
REGISTER_TEST(test_make_slug_collapse_and_trim);
