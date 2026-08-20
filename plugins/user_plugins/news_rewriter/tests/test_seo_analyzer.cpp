#include "test_framework.h"
#include "seo_analyzer.h"

using namespace news_rewriter;

static void test_seo_split_paragraphs() {
    auto p = SeoAnalyzer::split_paragraphs("Абзац один.\n\nАбзац два.\n\n\nТретий.");
    TEST_ASSERT_EQUAL(p.size(), 3u);
    TEST_ASSERT_EQUAL(p[0], "Абзац один.");
    TEST_ASSERT_EQUAL(p[2], "Третий.");
}

static void test_seo_split_sentences_abbr_and_decimal() {
    // "г." — сокращение, не конец предложения; "3.14" — десятичная дробь.
    auto s = SeoAnalyzer::split_sentences("Он живёт в г. Москве. Это факт.");
    TEST_ASSERT_EQUAL(s.size(), 2u);
    auto d = SeoAnalyzer::split_sentences("Цена 3.14 рубля. Купили товар.");
    TEST_ASSERT_EQUAL(d.size(), 2u);
}

static void test_seo_split_sentences_ellipsis() {
    // многоточие не разрывает предложение
    auto s = SeoAnalyzer::split_sentences("Ждали... Наконец пришли.");
    TEST_ASSERT_EQUAL(s.size(), 1u);
}

static void test_seo_split_words() {
    auto w = SeoAnalyzer::split_words("Привет, мир! Hello World.");
    // кириллица + латиница, без пунктуации, в нижнем регистре: 4 слова
    TEST_ASSERT_EQUAL(w.size(), 4u);
    TEST_ASSERT_EQUAL(w[0], "привет");
    TEST_ASSERT_EQUAL(w[1], "мир");
    TEST_ASSERT_EQUAL(w[2], "hello");
    TEST_ASSERT_EQUAL(w[3], "world");
}

static void test_seo_count_syllables() {
    TEST_ASSERT_EQUAL(SeoAnalyzer::count_syllables("hello", "en"), 2);
    TEST_ASSERT_EQUAL(SeoAnalyzer::count_syllables("привет", "ru"), 2);
    // минимум 1 слог даже для безгласных токенов
    TEST_ASSERT_EQUAL(SeoAnalyzer::count_syllables("xyz", "en"), 1);
}

static void test_seo_transition_word() {
    TEST_ASSERT_TRUE(SeoAnalyzer::has_transition_word("Кроме того, он пришёл.", "ru"));
    TEST_ASSERT_FALSE(SeoAnalyzer::has_transition_word("Он пошёл домой.", "ru"));
    TEST_ASSERT_TRUE(SeoAnalyzer::has_transition_word("However, it works.", "en"));
}

static void test_seo_passive() {
    TEST_ASSERT_TRUE(SeoAnalyzer::is_passive("Дом был построен.", "ru"));
    TEST_ASSERT_TRUE(SeoAnalyzer::is_passive("The book was written.", "en"));
    TEST_ASSERT_FALSE(SeoAnalyzer::is_passive("Он читает книгу.", "ru"));
    TEST_ASSERT_FALSE(SeoAnalyzer::is_passive("She reads a book.", "en"));
}

static void test_seo_flesch() {
    TEST_ASSERT_EQUAL(SeoAnalyzer::flesch("", "ru"), 0.0);
    // RU: читаемость — ASL-индекс (короткие предложения = легче).
    // Короткие предложения (~3-4 слова) дают высокий индекс.
    double short_r = SeoAnalyzer::flesch("Привет. Как у тебя дела?", "ru");
    // Длинные предложения (много слов, мало точек) дают низкий индекс.
    double long_r = SeoAnalyzer::flesch(
        "Это очень длинное предложение состоящее из множества слов которые "
        "идут друг за другом без единой точки и продолжают тянуться дальше "
        "перечисляя всё новые и новые обстоятельства событий происходящих "
        "вокруг нас каждый день и никогда не заканчиваются поскольку автор "
        "решил проверить поведение индекса удобочитаемости на практике.", "ru");
    TEST_ASSERT_TRUE(short_r > long_r);
    TEST_ASSERT_TRUE(short_r > 0.0);
    TEST_ASSERT_TRUE(short_r <= 100.0);
    TEST_ASSERT_TRUE(long_r >= 0.0);
    // EN: классический Flesch даёт положительное значение на простом тексте.
    double en = SeoAnalyzer::flesch("The cat sat on the mat. It was red.", "en");
    TEST_ASSERT_TRUE(en > 0.0);
}

static void test_seo_extract_headings() {
    std::string body = "# Заголовок раздела\n\nТекст абзаца.\n\n# Ещё один";
    auto h = SeoAnalyzer::extract_headings(body);
    TEST_ASSERT_EQUAL(h.size(), 2u);
    TEST_ASSERT_EQUAL(h[0], "Заголовок раздела");
    TEST_ASSERT_EQUAL(h[1], "Ещё один");
}

static void test_seo_analyze_basic() {
    SeoCriteria crit;
    std::string body =
        "# Устранение расхождений данных\n\n"
        "Устранение расхождений в данных важно. Кроме того, это упрощает "
        "контроль. Новый стандарт вступит в силу осенью.\n\n"
        "Исследователи создали эталон. Он был проверен лабораториями.";
    std::string title = "устранение расхождений данных: стандарт";
    std::string kp = "устранение расхождений";
    SeoReport r = SeoAnalyzer::analyze(body, title, kp, "ru", crit);
    TEST_ASSERT_FALSE(r.metrics.empty());
    TEST_ASSERT_TRUE(r.score >= 0 && r.score <= 100);
    // ключевая фраза есть в заголовке, 1-м абзаце и подзаголовке
    bool kp_title = false, kp_head = false;
    for (const auto& m : r.metrics) {
        if (m.key == "keyphrase_title" && m.status == SeoStatus::Good) kp_title = true;
        if (m.key == "keyphrase_heading" && m.status == SeoStatus::Good) kp_head = true;
    }
    TEST_ASSERT_TRUE(kp_title);
    TEST_ASSERT_TRUE(kp_head);
}

REGISTER_TEST(test_seo_split_paragraphs);
REGISTER_TEST(test_seo_split_sentences_abbr_and_decimal);
REGISTER_TEST(test_seo_split_sentences_ellipsis);
REGISTER_TEST(test_seo_split_words);
REGISTER_TEST(test_seo_count_syllables);
REGISTER_TEST(test_seo_transition_word);
REGISTER_TEST(test_seo_passive);
REGISTER_TEST(test_seo_flesch);
REGISTER_TEST(test_seo_extract_headings);
REGISTER_TEST(test_seo_analyze_basic);
