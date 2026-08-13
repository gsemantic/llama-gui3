#include "test_framework.h"

#include <cstdint>
#include <string>
#include <vector>

#include "sink.h"
#include "dotenv.h"
#include "storage.h"
#include "test_server.h"

using namespace news_rewriter;
using namespace news_rewriter_test;

namespace {

// Локальная копия base64 (должна совпадать с sink_wordpress.cpp) для проверки
// заголовка Authorization.
std::string base64_encode(const std::string& in) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    while (i + 2 < in.size()) {
        const uint32_t n = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) |
                           uint8_t(in[i + 2]);
        out += tbl[(n >> 18) & 0x3F];
        out += tbl[(n >> 12) & 0x3F];
        out += tbl[(n >> 6) & 0x3F];
        out += tbl[n & 0x3F];
        i += 3;
    }
    const std::size_t rem = in.size() - i;
    if (rem == 1) {
        const uint32_t n = uint8_t(in[i]) << 16;
        out += tbl[(n >> 18) & 0x3F];
        out += tbl[(n >> 12) & 0x3F];
        out += "==";
    } else if (rem == 2) {
        const uint32_t n = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8);
        out += tbl[(n >> 18) & 0x3F];
        out += tbl[(n >> 12) & 0x3F];
        out += tbl[(n >> 6) & 0x3F];
        out += "=";
    }
    return out;
}

Article make_article() {
    Article a;
    a.id = "abc123";
    a.url = "https://example.com/news/1";
    a.source = "example.com";
    a.fetched_at = "2026-08-08T12:00:00Z";
    a.title_original = "Заголовок";
    a.body_original = "Текст новости.";
    a.title_rewritten = "Новый заголовок";
    a.body_rewritten = "Новый текст.\n\nВторой абзац с <тегом> & амперсандом.";
    a.language = "ru";
    a.content_hash = "hash";
    return a;
}

SinkConfig make_config(const std::string& site_url,
                       const std::string& user = "publisher",
                       const std::string& pass = "secretapppass") {
    SinkConfig cfg;
    cfg.type = "wordpress";
    cfg.params = Json::object();
    cfg.params["site_url"] = site_url;
    cfg.params["username"] = user;
    cfg.params["app_password"] = pass;
    cfg.params["status"] = "draft";
    return cfg;
}

std::string post_path(const std::string& base) {
    return base + "/wp-json/wp/v2/posts";
}

} // namespace

// 7.1 + 7.2: успешное создание (201) с Basic-авторизацией и HTML-контентом.
static void test_wp_sink_created_201() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(201, "{\"id\":123}"));

    Storage storage;
    const auto sink = make_wordpress_sink(
        make_config(srv.base_url()), storage, nullptr);
    TEST_ASSERT_TRUE(sink != nullptr);
    TEST_ASSERT_EQUAL(std::string(sink->name()), "wordpress");
    TEST_ASSERT_TRUE(sink->write(make_article()));

    const std::string req = srv.last_request();
    TEST_ASSERT(req.find("POST /wp-json/wp/v2/posts") != std::string::npos);
    TEST_ASSERT(req.find("Content-Type: application/json") != std::string::npos);

    const std::string expected_auth =
        "Authorization: Basic " + base64_encode("publisher:secretapppass");
    TEST_ASSERT(req.find(expected_auth) != std::string::npos);

    // JSON-тело содержит title и HTML-контент.
    TEST_ASSERT(req.find("\"title\":\"Новый заголовок\"") != std::string::npos);
    TEST_ASSERT(req.find("<p>Новый текст.</p>") != std::string::npos);
    // Экранирование спецсимволов: сырые < и & не должны попасть как есть.
    TEST_ASSERT(req.find("<тегом>") == std::string::npos);
    TEST_ASSERT(req.find(" & амперсандом") == std::string::npos);
}

// 7.1: 401 (неверный пароль/пользователь) → неуспех, без бесконечных ретраев.
static void test_wp_sink_unauthorized_401() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(401, "{\"code\":\"rest_forbidden\"}"));

    Storage storage;
    const auto sink = make_wordpress_sink(
        make_config(srv.base_url()), storage, nullptr);
    TEST_ASSERT_FALSE(sink->write(make_article()));
    TEST_ASSERT_EQUAL(srv.request_count(), 1);   // без ретраев по умолчанию
}

// 7.1: 403 (нет прав edit_posts) и 400 (невалидное тело) → неуспех.
static void test_wp_sink_forbidden_and_bad_request() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(403, "{\"code\":\"rest_forbidden\"}"));

    Storage storage;
    const auto sink = make_wordpress_sink(
        make_config(srv.base_url()), storage, nullptr);
    TEST_ASSERT_FALSE(sink->write(make_article()));

    srv.stop();
    TEST_ASSERT_TRUE(srv.start(400, "{\"code\":\"rest_invalid\"}"));
    Storage storage2;
    const auto sink2 = make_wordpress_sink(
        make_config(srv.base_url()), storage2, nullptr);
    TEST_ASSERT_FALSE(sink2->write(make_article()));
}

// 7.1: сеть нестабильна (503) → ретраи по max_retries, затем неуспех.
static void test_wp_sink_retries_on_failure() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(503, "try later"));

    SinkConfig cfg = make_config(srv.base_url());
    cfg.params["max_retries"] = 2;
    cfg.params["retry_delay_ms"] = 0;

    Storage storage;
    const auto sink = make_wordpress_sink(cfg, storage, nullptr);
    TEST_ASSERT_FALSE(sink->write(make_article()));
    TEST_ASSERT_EQUAL(srv.request_count(), 3);   // 1 + 2 ретрая
}

// Пропуск при пустом рерайте (worker помечает Error, а не шлёт пустоту).
static void test_wp_sink_empty_rewrite() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(201, "{\"id\":1}"));

    Storage storage;
    const auto sink = make_wordpress_sink(
        make_config(srv.base_url()), storage, nullptr);
    Article a = make_article();
    a.title_rewritten.clear();
    TEST_ASSERT_FALSE(sink->write(a));
    TEST_ASSERT_EQUAL(srv.request_count(), 0);   // запрос не ушёл
}

// Валидация обязательных параметров (site_url/username/app_password).
static void test_wp_sink_missing_config() {
    Storage storage;
    SinkConfig cfg = make_config("");   // пустой site_url
    const auto sink = make_wordpress_sink(cfg, storage, nullptr);
    TEST_ASSERT_FALSE(sink->write(make_article()));
}

REGISTER_TEST(test_wp_sink_created_201);
REGISTER_TEST(test_wp_sink_unauthorized_401);
REGISTER_TEST(test_wp_sink_forbidden_and_bad_request);
REGISTER_TEST(test_wp_sink_retries_on_failure);
REGISTER_TEST(test_wp_sink_empty_rewrite);
REGISTER_TEST(test_wp_sink_missing_config);

// Произвольный тип записи (custom post type): endpoint строится по slug.
static void test_wp_sink_custom_post_type() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(201, "{\"id\":55}"));

    SinkConfig cfg = make_config(srv.base_url());
    cfg.params["post_type"] = "product";
    Json pcats = Json::array(); pcats.push(static_cast<int64_t>(7));
    cfg.params["categories"] = pcats;
    Json ptags = Json::array(); ptags.push(static_cast<int64_t>(9));
    cfg.params["tags"] = ptags;

    Storage storage;
    const auto sink = make_wordpress_sink(cfg, storage, nullptr);
    TEST_ASSERT_TRUE(sink->write(make_article()));

    const std::string req = srv.last_request();
    TEST_ASSERT(req.find("POST /wp-json/wp/v2/product") != std::string::npos);
    // Категории/теги шлются для произвольного типа (если CPT их поддерживает).
    TEST_ASSERT(req.find("\"categories\"") != std::string::npos);
    TEST_ASSERT(req.find("\"tags\"") != std::string::npos);
}

// Стандартные «страницы» (pages) не принимают категории/теги → не шлём их.
static void test_wp_sink_pages_skips_taxonomy() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(201, "{\"id\":77}"));

    SinkConfig cfg = make_config(srv.base_url());
    cfg.params["post_type"] = "pages";
    Json pcats = Json::array(); pcats.push(static_cast<int64_t>(3));
    cfg.params["categories"] = pcats;
    Json ptags = Json::array(); ptags.push(static_cast<int64_t>(4));
    cfg.params["tags"] = ptags;

    Storage storage;
    const auto sink = make_wordpress_sink(cfg, storage, nullptr);
    TEST_ASSERT_TRUE(sink->write(make_article()));

    const std::string req = srv.last_request();
    TEST_ASSERT(req.find("POST /wp-json/wp/v2/pages") != std::string::npos);
    TEST_ASSERT(req.find("\"categories\"") == std::string::npos);
    TEST_ASSERT(req.find("\"tags\"") == std::string::npos);
}

REGISTER_TEST(test_wp_sink_custom_post_type);
REGISTER_TEST(test_wp_sink_pages_skips_taxonomy);

// Тело опубликованного поста содержит hero-картинку (встроенную <img>) и ссылку
// на источник — иначе в выходной статье нет ни фото, ни ссылки (как в .md).
static void test_wp_sink_includes_image_and_source_link() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(201, "{\"id\":123}"));

    Storage storage;
    const auto sink = make_wordpress_sink(
        make_config(srv.base_url()), storage, nullptr);
    Article a = make_article();
    // Абсолютный URL картинки (на локальном сервере, чтобы не лезть в сеть).
    const std::string img = srv.base_url() + "/img.jpg";
    a.source_image = img;
    TEST_ASSERT_TRUE(sink->write(a));

    // last_request накапливает ВСЕ запросы, поэтому тело поста тоже внутри.
    const std::string req = srv.last_request();
    // hero-картинка встроена в контент.
    TEST_ASSERT(req.find("<img") != std::string::npos);
    TEST_ASSERT(req.find(img) != std::string::npos);
    // ссылка на источник в конце контента.
    TEST_ASSERT(req.find("Источник: <a href=") != std::string::npos);
    TEST_ASSERT(req.find("example.com</a>") != std::string::npos);
}

// Дата публикации оригинала попадает в тело опубликованного поста.
static void test_wp_sink_includes_original_date() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(201, "{\"id\":123}"));

    Storage storage;
    const auto sink = make_wordpress_sink(
        make_config(srv.base_url()), storage, nullptr);
    Article a = make_article();
    a.published_at = 1755000000;   // 2025-08-12T12:00:00Z
    TEST_ASSERT_TRUE(sink->write(a));

    const std::string req = srv.last_request();
    TEST_ASSERT(req.find("Дата оригинала: 2025-08-12T12:00:00Z") !=
                std::string::npos);
}

REGISTER_TEST(test_wp_sink_includes_image_and_source_link);
REGISTER_TEST(test_wp_sink_includes_original_date);

// Секрет берётся из .env, а не из params (конвенция проекта: секреты вне
// settings.ini). Проверяем, что в заголовке — creds из .env, а не из params.
static void test_wp_sink_credentials_from_env() {
    const std::string dir = "./nr_wp_env_test";
    Storage storage;
    TEST_ASSERT_TRUE(storage.init(dir));
    const std::string env_path = storage.root() + "/.env";
    dotenv_write(env_path, kNewsRewriterWpUser, "envuser");
    dotenv_write(env_path, kNewsRewriterWpPass, "envpass");

    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(201, "{\"id\":9}"));

    // В params — другие (ложные) creds; sink должен проигнорировать их.
    SinkConfig cfg = make_config(srv.base_url(), "paramsuser", "paramspass");
    const auto sink = make_wordpress_sink(cfg, storage, nullptr);
    TEST_ASSERT_TRUE(sink->write(make_article()));

    const std::string req = srv.last_request();
    const std::string expected = "Authorization: Basic " +
                                 base64_encode("envuser:envpass");
    TEST_ASSERT(req.find(expected) != std::string::npos);
    TEST_ASSERT(req.find(base64_encode("paramsuser:paramspass")) ==
                 std::string::npos);
}

REGISTER_TEST(test_wp_sink_credentials_from_env);

// Проверка подключения: 200 + имя пользователя.
static void test_wp_check_connection_ok() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(200, "{\"name\":\"Test User\"}"));
    const std::string res = wordpress_check_connection(
        srv.base_url(), "u", "p");
    TEST_ASSERT(res.rfind("OK", 0) == 0);
    TEST_ASSERT(res.find("Test User") != std::string::npos);
}

// Проверка подключения: 401 → ошибка авторизации.
static void test_wp_check_connection_unauthorized() {
    MiniHttpServer srv;
    TEST_ASSERT_TRUE(srv.start(401, "{\"code\":\"rest_forbidden\"}"));
    const std::string res = wordpress_check_connection(
        srv.base_url(), "u", "bad");
    TEST_ASSERT(res.find("ошибка авторизации") != std::string::npos);
}

// Проверка подключения: не заданы параметры.
static void test_wp_check_connection_missing() {
    const std::string res = wordpress_check_connection("", "", "");
    TEST_ASSERT(res.find("не заданы") != std::string::npos);
}

REGISTER_TEST(test_wp_check_connection_ok);
REGISTER_TEST(test_wp_check_connection_unauthorized);
REGISTER_TEST(test_wp_check_connection_missing);
