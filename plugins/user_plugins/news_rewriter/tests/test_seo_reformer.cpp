#include "test_framework.h"
#include "seo_reformer.h"
#include "seo_analyzer.h"

using namespace news_rewriter;

static void test_reformer_split_paragraphs() {
    // 6 коротких предложений по 1 слову, без пустых строк — один абзац.
    // При max=2 слов должен разбиться на 3 абзаца.
    std::string body = "Одно. Два. Три. Четыре. Пять. Шесть.";
    std::string out = SeoReformer::split_long_paragraphs(body, 2);
    auto paras = SeoAnalyzer::split_paragraphs(out);
    TEST_ASSERT_EQUAL(paras.size(), 3u);
    // каждый получившийся абзац <= 2 слов
    for (const auto& p : paras)
        TEST_ASSERT_TRUE(SeoAnalyzer::split_words(p).size() <= 2u);
}

static void test_reformer_split_paragraphs_keeps_heading() {
    std::string body = "# Мой раздел\n\nПервое. Второе. Третье. Четвёртое. Пятое. Шестое.";
    std::string out = SeoReformer::split_long_paragraphs(body, 2);
    auto paras = SeoAnalyzer::split_paragraphs(out);
    // заголовок остаётся отдельным абзацем, текст разбит на 3
    TEST_ASSERT_EQUAL(paras.size(), 4u);
    TEST_ASSERT_EQUAL(paras[0], "# Мой раздел");
}

static void test_reformer_split_long_sentence() {
    std::string s = "Мы пошли в лес, нашли гриб, сварили суп, "
                    "позвали друзей, поели и легли спать.";
    std::string out = SeoReformer::split_long_sentence(s, 5, "ru");
    // разбито на несколько предложений, каждое <= 5 слов
    auto sents = SeoAnalyzer::split_sentences(out);
    TEST_ASSERT_TRUE(sents.size() >= 2u);
    for (const auto& p : sents)
        TEST_ASSERT_TRUE(SeoAnalyzer::split_words(p).size() <= 5u);
}

static void test_reformer_split_long_sentence_no_comma() {
    // без запятых механически разбить нельзя — возвращается как есть
    std::string s = "Этопредложениевообщебезпробеловичереззапятыхоченьдлинное";
    std::string out = SeoReformer::split_long_sentence(s, 5, "ru");
    TEST_ASSERT_EQUAL(out, s);
}

static void test_reformer_add_transition() {
    std::string a = SeoReformer::add_transition("Он пошёл домой.", "ru");
    TEST_ASSERT_TRUE(a.find("Кроме того") == 0);
    // идемпотентно: повторный вызов не добавляет второе переходное слово
    std::string b = SeoReformer::add_transition(a, "ru");
    TEST_ASSERT_EQUAL(b, a);
    // если переходное слово уже есть — не трогаем
    std::string c = SeoReformer::add_transition("Кроме того, он пришёл.", "ru");
    TEST_ASSERT_EQUAL(c, "Кроме того, он пришёл.");
}

static void test_reformer_reform_end_to_end() {
    std::string body =
        "# Ремонт квартир своими руками\n\n"
        "Ремонт квартиры требует планирования, закупки материалов, "
        "найма бригады, согласования сметы, контроля сроков и приёмки работ, "
        "поэтому лучше начать с составления подробного графика и бюджета.\n\n"
        "Мастера выравнивают стены, клеят обои, стелят пол, "
        "меняют проводку, устанавливают сантехнику и убирают мусор.";

    SeoReformConfig cfg;          // defaults: paragraphs 120, sentences 25
    cfg.max_sentence_words = 20;  // искусственно жёстко, чтобы сработало дробление
    SeoReformResult res = SeoReformer::reform(body, cfg);

    // заголовок не тронут
    TEST_ASSERT_TRUE(res.reformed_body.find("# Ремонт квартир") != std::string::npos);
    // длинные предложения разбиты
    TEST_ASSERT_TRUE(res.sentences_split > 0);
    // после реформы ни одно предложение не длиннее порога
    auto sents = SeoAnalyzer::split_sentences(res.reformed_body);
    for (const auto& s : sents)
        TEST_ASSERT_TRUE(SeoAnalyzer::split_words(s).size() <= cfg.max_sentence_words);
}

static void test_reformer_noop_when_within_limits() {
    std::string body = "Коротко. Ещё короче.";
    SeoReformResult res = SeoReformer::reform(body);
    TEST_ASSERT_EQUAL(res.sentences_split, 0);
    TEST_ASSERT_EQUAL(res.paragraphs_split, 0);
    TEST_ASSERT_EQUAL(res.reformed_body, body);
}

REGISTER_TEST(test_reformer_split_paragraphs);
REGISTER_TEST(test_reformer_split_paragraphs_keeps_heading);
REGISTER_TEST(test_reformer_split_long_sentence);
REGISTER_TEST(test_reformer_split_long_sentence_no_comma);
REGISTER_TEST(test_reformer_add_transition);
REGISTER_TEST(test_reformer_reform_end_to_end);
REGISTER_TEST(test_reformer_noop_when_within_limits);
