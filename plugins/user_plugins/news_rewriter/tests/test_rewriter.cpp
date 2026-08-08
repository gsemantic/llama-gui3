#include "test_framework.h"

#include <string>

#include "rewriter.h"

using namespace news_rewriter;

namespace {

static Article make_article(const std::string& title, const std::string& body) {
    Article a;
    a.id = "id";
    a.url = "https://a.example/news/1";
    a.source = "a.example";
    a.title_original = title;
    a.body_original = body;
    a.language = "ru";
    return a;
}

static void test_build_prompt_substitutions() {
    const Article a = make_article("Заголовок", "Тело новости");
    RewriteConfig cfg;
    cfg.prompt_template = "Язык: {language}; Тон: {tone}; Заголовок: {title}; Текст: {body}";
    const std::string prompt = build_prompt(a, cfg);
    TEST_ASSERT(prompt.find("Язык: ru") != std::string::npos);
    TEST_ASSERT(prompt.find("Тон: нейтральный") != std::string::npos);
    TEST_ASSERT(prompt.find("Заголовок: Заголовок") != std::string::npos);
    TEST_ASSERT(prompt.find("Текст: Тело новости") != std::string::npos);
}

static void test_parse_response_title_and_body() {
    const RewriteResult r = parse_response("Новый заголовок\n\nНовый текст статьи\nВторая строка");
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL(r.title, "Новый заголовок");
    TEST_ASSERT_EQUAL(r.body, "Новый текст статьи\nВторая строка");
}

static void test_parse_response_single_line() {
    const RewriteResult r = parse_response("Только одна строка без заголовка");
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL(r.title, "Только одна строка без заголовка");
    TEST_ASSERT_TRUE(r.body.empty());
}

static void test_parse_response_empty() {
    const RewriteResult r = parse_response("");
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_FALSE(r.error.empty());
}

static void test_parse_response_whitespace_only() {
    const RewriteResult r = parse_response("   \n  \n");
    TEST_ASSERT_FALSE(r.ok);
}

static void test_rewrite_ok() {
    const Article a = make_article("Старый заголовок", "Старый текст");
    RewriteConfig cfg;
    LlmFn llm = [](const std::string&, std::string& response, std::string&) -> bool {
        response = "Переписанный заголовок\n\nПереписанный текст новости";
        return true;
    };
    const RewriteResult r = rewrite_article(a, cfg, llm);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL(r.title, "Переписанный заголовок");
    TEST_ASSERT_EQUAL(r.body, "Переписанный текст новости");
}

static void test_rewrite_llm_error() {
    const Article a = make_article("Заголовок", "Текст");
    RewriteConfig cfg;
    LlmFn llm = [](const std::string&, std::string&, std::string& error) -> bool {
        error = "сервер недоступен";
        return false;
    };
    const RewriteResult r = rewrite_article(a, cfg, llm);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL(r.error, "сервер недоступен");
}

static void test_rewrite_no_llm() {
    const Article a = make_article("Заголовок", "Текст");
    RewriteConfig cfg;
    const RewriteResult r = rewrite_article(a, cfg, LlmFn{});
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_FALSE(r.error.empty());
}

REGISTER_TEST(test_build_prompt_substitutions);
REGISTER_TEST(test_parse_response_title_and_body);
REGISTER_TEST(test_parse_response_single_line);
REGISTER_TEST(test_parse_response_empty);
REGISTER_TEST(test_parse_response_whitespace_only);
REGISTER_TEST(test_rewrite_ok);
REGISTER_TEST(test_rewrite_llm_error);
REGISTER_TEST(test_rewrite_no_llm);

} // namespace
