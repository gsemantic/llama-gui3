#include "test_framework.h"

#include <string>

#include "url_norm.h"

using namespace news_rewriter;

// IDN-домен (кириллица в хосте) перекодируется в punycode.
static void test_url_norm_idn_host() {
    TEST_ASSERT_EQUAL(
        normalize_url("https://президент.рф/"),
        "https://xn--d1abbgf6aiiy.xn--p1ai/");
    TEST_ASSERT_EQUAL(
        normalize_url("https://пример.рф/новости?q=1"),
        "https://xn--e1afmkfd.xn--p1ai/новости?q=1");
}

// Порт, userinfo и смешанные метки сохраняются корректно.
static void test_url_norm_idn_port_userinfo() {
    TEST_ASSERT_EQUAL(
        normalize_url("https://пример.рф:8080/"),
        "https://xn--e1afmkfd.xn--p1ai:8080/");
    TEST_ASSERT_EQUAL(
        normalize_url("https://user:pass@кто.рф/x"),
        "https://user:pass@xn--j1ail.xn--p1ai/x");
}

// Обычные ASCII-хосты и уже закодированные punycode не трогаются.
static void test_url_norm_ascii_unchanged() {
    TEST_ASSERT_EQUAL(
        normalize_url("https://example.com/path?a=b#frag"),
        "https://example.com/path?a=b#frag");
    TEST_ASSERT_EQUAL(
        normalize_url("https://xn--e1afmkfd.xn--p1ai/"),
        "https://xn--e1afmkfd.xn--p1ai/");
}

// Без схемы (relative) хост не выделяем — возвращаем как есть.
static void test_url_norm_no_scheme_unchanged() {
    TEST_ASSERT_EQUAL(normalize_url("//президент.рф/path"),
                      "//президент.рф/path");
    TEST_ASSERT_EQUAL(normalize_url("/новости/статья"),
                      "/новости/статья");
}

// IPv6-литералы в квадратных скобках не ломаются.
static void test_url_norm_ipv6_literal() {
    TEST_ASSERT_EQUAL(normalize_url("https://[2001:db8::1]/path"),
                      "https://[2001:db8::1]/path");
}

REGISTER_TEST(test_url_norm_idn_host);
REGISTER_TEST(test_url_norm_idn_port_userinfo);
REGISTER_TEST(test_url_norm_ascii_unchanged);
REGISTER_TEST(test_url_norm_no_scheme_unchanged);
REGISTER_TEST(test_url_norm_ipv6_literal);
