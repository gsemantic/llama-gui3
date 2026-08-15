#include "test_framework.h"

#include <string>

#include "config.h"
#include "headless.h"

using namespace news_rewriter;

namespace {

// «Оболочка» SPA: почти нет видимого текста (как у VK.ru до исполнения JS).
static void test_headless_thin_content_shell() {
    const std::string shell =
        "<html><head><title>ВКонтакте</title></head>"
        "<body><div id=\"app\"></div>"
        "<script>var a='привет';</script></body></html>";
    HeadlessRenderer r;
    TEST_ASSERT_TRUE(r.is_thin_content(shell));
}

// Реальная статья: много видимого текста — не «оболочка».
static void test_headless_thin_content_article() {
    std::string body = "<html><body><article>";
    for (int i = 0; i < 60; ++i) body += "<p>Это текст новости про события дня.</p>";
    body += "</article></body></html>";
    HeadlessRenderer r;
    TEST_ASSERT_FALSE(r.is_thin_content(body));
}

// Если Chromium доступен, реально рендерим data:-URL (без сети) и проверяем,
// что DOM содержит исполненный контент. Без браузера тест проходит условно.
static void test_headless_render_data_url() {
    NetworkConfig cfg;
    HeadlessRenderer r;
    if (!r.available(cfg)) {
        TEST_ASSERT_TRUE(true);  // браузер не установлен — пропускаем
        return;
    }
    const std::string url =
        "data:text/html,<html><head><meta charset=utf-8><title>Тест</title></head>"
        "<body><article><h1>Заголовок</h1><p>Привет мир статья текст.</p></article>"
        "</body></html>";
    std::string err;
    const std::string dom = r.render(url, cfg, &err);
    TEST_ASSERT_FALSE(dom.empty());
    TEST_ASSERT_TRUE(dom.find("Привет мир статья текст.") != std::string::npos);
}

} // namespace

REGISTER_TEST(test_headless_thin_content_shell);
REGISTER_TEST(test_headless_thin_content_article);
REGISTER_TEST(test_headless_render_data_url);
