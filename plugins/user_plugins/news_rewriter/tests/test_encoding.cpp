#include "test_framework.h"

#include <string>

#include "common.h"

using namespace news_rewriter;

namespace {

// "Привет" в CP1251: П=0xCF р=0xF0 и=0xE8 в=0xE2 е=0xE5 т=0xF2.
const std::string kPrivetCp1251 = std::string("\xCF\xF0\xE8\xE2\xE5\xF2", 6);
const std::string kPrivetUtf8 = "Привет";

static void test_cp1251_to_utf8_basic() {
    TEST_ASSERT_EQUAL(to_utf8(kPrivetCp1251, "text/html; charset=windows-1251"),
                     kPrivetUtf8);
}

static void test_cp1251_meta_charset() {
    // Мета-тег внутри тела: декларируемая кодировка берётся из самого текста.
    const std::string html = std::string("<html><head><meta charset=\"windows-1251\">")
        + kPrivetCp1251 + "</body>";
    const std::string expect = std::string("<html><head><meta charset=\"windows-1251\">")
        + kPrivetUtf8 + "</body>";
    TEST_ASSERT_EQUAL(to_utf8(html, ""), expect);
}

static void test_cp1251_http_equiv_meta() {
    const std::string html =
        std::string("<meta http-equiv=\"Content-Type\" content=\"text/html; charset=cp1251\">")
        + kPrivetCp1251;
    const std::string expect =
        std::string("<meta http-equiv=\"Content-Type\" content=\"text/html; charset=cp1251\">")
        + kPrivetUtf8;
    TEST_ASSERT_EQUAL(to_utf8(html, ""), expect);
}

static void test_utf8_passthrough() {
    // Для UTF-8/пустого заголовка текст возвращается без изменений.
    TEST_ASSERT_EQUAL(to_utf8(kPrivetUtf8, "text/html; charset=utf-8"),
                     kPrivetUtf8);
    TEST_ASSERT_EQUAL(to_utf8(kPrivetUtf8, ""), kPrivetUtf8);
}

static void test_unknown_charset_passthrough() {
    // Неизвестная кодировка — возвращаем как есть (не портим UTF-8).
    TEST_ASSERT_EQUAL(to_utf8(kPrivetUtf8, "text/html; charset=iso-8859-1"),
                     kPrivetUtf8);
}

static void test_iso8859_5_to_utf8() {
    // iso-8859-5: А=0xB0 (→U+0410), а=0xD0 (→U+0430).
    const std::string sample = std::string("\xB0\xD0", 2);
    const std::string expect = "Аа";
    TEST_ASSERT_EQUAL(to_utf8(sample, "text/html; charset=iso-8859-5"), expect);
}

} // namespace

REGISTER_TEST(test_cp1251_to_utf8_basic);
REGISTER_TEST(test_cp1251_meta_charset);
REGISTER_TEST(test_cp1251_http_equiv_meta);
REGISTER_TEST(test_utf8_passthrough);
REGISTER_TEST(test_unknown_charset_passthrough);
REGISTER_TEST(test_iso8859_5_to_utf8);
