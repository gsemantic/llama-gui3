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

static void test_parse_response_long_single_line_is_body() {
    const std::string long_line(200, 'x');
    const RewriteResult r = parse_response(long_line);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_TRUE(r.title.empty());
    TEST_ASSERT_EQUAL(r.body, long_line);
}

// Регрессия: длинная ПЕРВАЯ строка (целый абзац) + ещё строки. Заголовком она
// быть не должна — иначе в заголовок попадает половина текста статьи (см. баг
// «Технофашисты XXI века» после rate-limit). Заголовок пуст, тело = все строки.
static void test_parse_response_long_first_line_with_more_is_body() {
    const std::string long_line(400, 'x');
    const RewriteResult r = parse_response(long_line + "\nвторая строка\nтретья");
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_TRUE(r.title.empty());
    TEST_ASSERT_EQUAL(r.body, long_line + "\nвторая строка\nтретья");
}

static void test_rewrite_single_long_paragraph() {
    const Article a = make_article("Старый заголовок", "Старый текст");
    RewriteConfig cfg;
    cfg.language = "";  // без проверки языка — тест проверяет только разбор
    const std::string text(200, 'a');
    LlmFn llm = [&](const std::string& system, const std::string& user,
                   std::string& response, std::string&) -> bool {
        response = text;
        return true;
    };
    const RewriteResult r = rewrite_article(a, cfg, llm);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_TRUE(r.title.empty());
    TEST_ASSERT_EQUAL(r.body, text);
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
    LlmFn llm = [](const std::string& system, const std::string& user,
                   std::string& response, std::string&) -> bool {
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
    LlmFn llm = [](const std::string& system, const std::string& user,
                   std::string&, std::string& error) -> bool {
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

static void test_build_prompt_max_words_appends_instruction() {
    const Article a = make_article("Заголовок", "Тело");
    RewriteConfig cfg;
    cfg.max_words = 120;
    const std::string prompt = build_prompt(a, cfg);
    TEST_ASSERT(prompt.find("Объём: примерно 120 слов.") != std::string::npos);
}

static void test_build_prompt_max_words_zero_no_instruction() {
    const Article a = make_article("Заголовок", "Тело");
    RewriteConfig cfg;
    cfg.max_words = 0;
    const std::string prompt = build_prompt(a, cfg);
    TEST_ASSERT(prompt.find("Объём:") == std::string::npos);
}

static void test_build_prompt_max_words_placeholder() {
    const Article a = make_article("Заголовок", "Тело");
    RewriteConfig cfg;
    cfg.max_words = 75;
    cfg.prompt_template = "Напиши {max_words} слов.";
    const std::string prompt = build_prompt(a, cfg);
    TEST_ASSERT(prompt.find("{max_words}") == std::string::npos);
    TEST_ASSERT(prompt.find("Напиши 75 слов.") != std::string::npos);
}

// Регрессия: перегруженная модель вернула 200 с латиницей вместо русского
// рерайта — такой «деградировавший» ответ не должен публиковаться.
static void test_validate_rewrite_wrong_script_rejected() {
    std::string err;
    const bool ok = validate_rewrite("Title", "Some English body text.",
                                     "ru", err);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_FALSE(err.empty());
}

static void test_validate_rewrite_refusal_rejected() {
    std::string err;
    const bool ok = validate_rewrite("Извините", "как языковая модель, я не могу",
                                     "ru", err);
    TEST_ASSERT_FALSE(ok);
}

static void test_validate_rewrite_valid_cyrillic_ok() {
    std::string err;
    const bool ok = validate_rewrite("Переписанный заголовок",
                                     "Переписанный текст новости на русском.",
                                     "ru", err);
    TEST_ASSERT_TRUE(ok);
}

static void test_validate_rewrite_unknown_lang_skips_script_check() {
    std::string err;
    const bool ok = validate_rewrite("Title", "English body", "", err);
    TEST_ASSERT_TRUE(ok);
}

REGISTER_TEST(test_build_prompt_substitutions);
REGISTER_TEST(test_validate_rewrite_wrong_script_rejected);
REGISTER_TEST(test_validate_rewrite_refusal_rejected);
REGISTER_TEST(test_validate_rewrite_valid_cyrillic_ok);
REGISTER_TEST(test_validate_rewrite_unknown_lang_skips_script_check);
REGISTER_TEST(test_parse_response_title_and_body);
REGISTER_TEST(test_parse_response_single_line);
REGISTER_TEST(test_parse_response_long_single_line_is_body);
REGISTER_TEST(test_parse_response_long_first_line_with_more_is_body);
REGISTER_TEST(test_rewrite_single_long_paragraph);
REGISTER_TEST(test_parse_response_empty);
REGISTER_TEST(test_parse_response_whitespace_only);
REGISTER_TEST(test_rewrite_ok);
REGISTER_TEST(test_rewrite_llm_error);
REGISTER_TEST(test_rewrite_no_llm);
REGISTER_TEST(test_build_prompt_max_words_appends_instruction);
REGISTER_TEST(test_build_prompt_max_words_zero_no_instruction);
REGISTER_TEST(test_build_prompt_max_words_placeholder);

} // namespace
