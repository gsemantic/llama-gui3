#include "test_framework.h"

#include <string>

#include "http.h"
#include "test_server.h"

using namespace news_rewriter;
using namespace news_rewriter_test;

static void test_http_get_basic() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(200, "hello world"));
    HttpClient http;
    TEST_ASSERT_TRUE(http.init());
    NetworkConfig cfg;
    const HttpResponse r = http.get(srv.base_url() + "/path", cfg);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL(r.status, 200);
    TEST_ASSERT_EQUAL(r.body, "hello world");
    TEST_ASSERT_FALSE(r.final_url.empty());
}

static void test_http_not_found() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(404, "not found"));
    HttpClient http;
    TEST_ASSERT_TRUE(http.init());
    NetworkConfig cfg;
    const HttpResponse r = http.get(srv.base_url() + "/missing", cfg);
    TEST_ASSERT_TRUE(r.ok);          // транспорт успешен
    TEST_ASSERT_EQUAL(r.status, 404);
}

static void test_http_connection_error() {
    // Порт, на котором никто не слушает.
    MiniHttpServer srv;
    HttpClient http;
    TEST_ASSERT_TRUE(http.init());
    NetworkConfig cfg;
    cfg.timeout_seconds = 1;
    const HttpResponse r = http.get("http://127.0.0.1:1/", cfg);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_FALSE(r.error.empty());
}

static void test_http_timeout() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(200, "slow response", "text/plain", 3000));
    HttpClient http;
    TEST_ASSERT_TRUE(http.init());
    NetworkConfig cfg;
    cfg.timeout_seconds = 1;
    const HttpResponse r = http.get(srv.base_url() + "/slow", cfg);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_FALSE(r.error.empty());
}

static void test_http_size_limit() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(200, std::string(100, 'x')));
    HttpClient http;
    TEST_ASSERT_TRUE(http.init());
    NetworkConfig cfg;
    const HttpResponse r = http.get(srv.base_url() + "/big", cfg, 50);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_TRUE(r.error.find("лимит") != std::string::npos);
}

static void test_http_user_agent_sent() {
    // Проверяем только, что запрос с UA выполняется без ошибок.
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(200, "ok"));
    HttpClient http;
    TEST_ASSERT_TRUE(http.init());
    NetworkConfig cfg;
    cfg.user_agent = "news_rewriter-test/1.0";
    const HttpResponse r = http.get(srv.base_url(), cfg);
    TEST_ASSERT_TRUE(r.ok);
}

// Кастомные заголовки из NetworkConfig.extra_headers доходят до сервера.
static void test_http_extra_headers_sent() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(200, "ok"));
    HttpClient http;
    TEST_ASSERT_TRUE(http.init());
    NetworkConfig cfg;
    cfg.extra_headers = "X-Custom-Header: 12345\nAuthorization: Bearer get-token";
    const HttpResponse r = http.get(srv.base_url() + "/auth", cfg);
    TEST_ASSERT_TRUE(r.ok);
    const std::string req = srv.last_request();
    TEST_ASSERT(req.find("X-Custom-Header: 12345") != std::string::npos);
    TEST_ASSERT(req.find("Authorization: Bearer get-token") != std::string::npos);
}

// Указанный proxy реально используется: curl шлёт абсолютную форму запроса
// (GET http://host/path HTTP/1.1) на локальный «прокси» вместо прямого соединения.
static void test_http_proxy_used() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(200, "proxied"));
    HttpClient http;
    TEST_ASSERT_TRUE(http.init());
    NetworkConfig cfg;
    cfg.proxy = srv.base_url();   // локальный сервер играет роль прокси
    const HttpResponse r = http.get("http://example.invalid/page", cfg);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL(r.body, "proxied");
    const std::string req = srv.last_request();
    TEST_ASSERT(req.find("GET http://example.invalid/page HTTP/1.1") != std::string::npos);
}

REGISTER_TEST(test_http_get_basic);
REGISTER_TEST(test_http_not_found);
REGISTER_TEST(test_http_connection_error);
REGISTER_TEST(test_http_timeout);
REGISTER_TEST(test_http_size_limit);
REGISTER_TEST(test_http_user_agent_sent);
REGISTER_TEST(test_http_extra_headers_sent);
REGISTER_TEST(test_http_proxy_used);
