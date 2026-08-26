#include "../../include/core/rag_manager.h"
#include "../../include/core/stemmer.h"
#include "../test_framework.h"

using namespace llama_gui::core;

// Create a RagManager instance for testing utility methods
static RagManager* rag = nullptr;

RagManager* get_rag() {
    if (!rag) {
        rag = new RagManager("");
    }
    return rag;
}

// ============================================================================
// Tests for count_tokens_approx
// ============================================================================

void test_count_tokens_empty() {
    TEST_ASSERT_EQUAL(get_rag()->count_tokens_approx(""), 0);
}

void test_count_tokens_english_single_word() {
    TEST_ASSERT_EQUAL(get_rag()->count_tokens_approx("hello"), 1);
}

void test_count_tokens_english_multiple_words() {
    TEST_ASSERT_EQUAL(get_rag()->count_tokens_approx("hello world"), 2);
    TEST_ASSERT_EQUAL(get_rag()->count_tokens_approx("the quick brown fox"), 4);
}

void test_count_tokens_english_with_punctuation() {
    int tokens = get_rag()->count_tokens_approx("hello, world!");
    TEST_ASSERT(tokens >= 2 && tokens <= 3);
}

void test_count_tokens_russian_single_word() {
    int tokens = get_rag()->count_tokens_approx("привет");
    TEST_ASSERT(tokens >= 1 && tokens <= 2);
}

void test_count_tokens_russian_multiple_words() {
    int tokens = get_rag()->count_tokens_approx("привет мир");
    std::cout << "[DEBUG] count_tokens_approx('привет мир') = " << tokens << std::endl;
    TEST_ASSERT(tokens >= 2 && tokens <= 3);
}

void test_count_tokens_russian_long_sentence() {
    std::string text = "Машина ехала быстро по дороге к дому.";
    int tokens = get_rag()->count_tokens_approx(text);
    TEST_ASSERT(tokens >= 5 && tokens <= 10);
}

void test_count_tokens_mixed_language() {
    int tokens = get_rag()->count_tokens_approx("hello привет world мир");
    TEST_ASSERT(tokens >= 3 && tokens <= 5);
}

// ============================================================================
// Tests for split_into_chunks
// ============================================================================

void test_split_empty_text() {
    auto chunks = get_rag()->split_into_chunks("", 100);
    TEST_ASSERT_EQUAL(chunks.size(), 0u);
}

void test_split_single_sentence() {
    auto chunks = get_rag()->split_into_chunks("Hello world.", 100);
    std::cout << "[DEBUG] split_into_chunks('Hello world.', 100) = " << chunks.size() << " chunks" << std::endl;
    if (!chunks.empty()) std::cout << "[DEBUG]   chunk[0] = '" << chunks[0] << "'" << std::endl;
    TEST_ASSERT_EQUAL(chunks.size(), 1u);
    TEST_ASSERT_EQUAL(chunks[0], "Hello world.");
}

void test_split_multiple_sentences_english() {
    auto chunks = get_rag()->split_into_chunks("First sentence. Second sentence. Third sentence.", 100);
    TEST_ASSERT(chunks.size() >= 1);
    std::string combined;
    for (const auto& c : chunks) combined += c + " ";
    TEST_ASSERT(combined.find("First sentence") != std::string::npos);
    TEST_ASSERT(combined.find("Second sentence") != std::string::npos);
}

void test_split_by_exclamation() {
    auto chunks = get_rag()->split_into_chunks("Hello! World!", 100);
    TEST_ASSERT_EQUAL(chunks.size(), 1u);
}

void test_split_by_question() {
    auto chunks = get_rag()->split_into_chunks("How are you? I am fine.", 100);
    TEST_ASSERT_EQUAL(chunks.size(), 1u);
}

void test_split_russian_sentences() {
    std::string text = "Машина ехала быстро. Документ был готов. Всё хорошо.";
    auto chunks = get_rag()->split_into_chunks(text, 5);
    std::cout << "[DEBUG] Russian 3 sentences (max=5): " << chunks.size() << " chunks" << std::endl;
    for (size_t i = 0; i < chunks.size(); i++) {
        std::cout << "[DEBUG]   chunk[" << i << "] = '" << chunks[i] << "'" << std::endl;
    }
    TEST_ASSERT(chunks.size() >= 2);
}

void test_split_russian_with_dash() {
    std::string text = "Было три варианта — первый, второй и третий.";
    auto chunks = get_rag()->split_into_chunks(text, 100);
    TEST_ASSERT(chunks.size() >= 1);
}

void test_split_ellipsis_not_boundary() {
    std::string text = "Подождите... Потом продолжим.";
    auto chunks = get_rag()->split_into_chunks(text, 100);
    TEST_ASSERT_EQUAL(chunks.size(), 1u);
}

void test_split_long_sentence_by_commas() {
    std::string text = "Один, два, три, четыре, пять, шесть, семь, восемь, девять, десять.";
    auto chunks = get_rag()->split_into_chunks(text, 5);
    TEST_ASSERT(chunks.size() >= 2);
}

// ============================================================================
// Tests for Stemmer
// ============================================================================

void test_stemmer_is_russian() {
    TEST_ASSERT_TRUE(Stemmer::is_russian("привет"));
    TEST_ASSERT_TRUE(Stemmer::is_russian("hello мир"));
    TEST_ASSERT_FALSE(Stemmer::is_russian("hello"));
    TEST_ASSERT_FALSE(Stemmer::is_russian("12345"));
}

void test_stemmer_is_english() {
    TEST_ASSERT_TRUE(Stemmer::is_english("hello"));
    TEST_ASSERT_TRUE(Stemmer::is_english("привет hello"));
    TEST_ASSERT_FALSE(Stemmer::is_english("привет"));
}

void test_stemmer_russian_basic() {
    std::cout << "[DEBUG] stem_russian('машине') = '" << Stemmer::stem_russian("машине") << "'" << std::endl;
    std::cout << "[DEBUG] stem_russian('машину') = '" << Stemmer::stem_russian("машину") << "'" << std::endl;
    std::cout << "[DEBUG] stem_russian('машиной') = '" << Stemmer::stem_russian("машиной") << "'" << std::endl;
    TEST_ASSERT_EQUAL(Stemmer::stem_russian("машине"), std::string("машин"));
    TEST_ASSERT_EQUAL(Stemmer::stem_russian("машину"), std::string("машин"));
    TEST_ASSERT_EQUAL(Stemmer::stem_russian("машиной"), std::string("машин"));
}

void test_stemmer_russian_short_words() {
    TEST_ASSERT_EQUAL(Stemmer::stem_russian("дом"), std::string("дом"));
    TEST_ASSERT_EQUAL(Stemmer::stem_russian("кот"), std::string("кот"));
}

void test_stemmer_russian_verbs() {
    std::string stem = Stemmer::stem_russian("читать");
    TEST_ASSERT(stem.length() >= 3);
}

void test_stemmer_english_basic() {
    TEST_ASSERT_EQUAL(Stemmer::stem_english("running"), std::string("runn"));
    TEST_ASSERT_EQUAL(Stemmer::stem_english("teacher"), std::string("teach"));
    TEST_ASSERT_EQUAL(Stemmer::stem_english("beautiful"), std::string("beauti"));
}

void test_stemmer_english_short_words() {
    TEST_ASSERT_EQUAL(Stemmer::stem_english("cat"), std::string("cat"));
    TEST_ASSERT_EQUAL(Stemmer::stem_english("dog"), std::string("dog"));
}

void test_stemmer_auto_detect() {
    TEST_ASSERT_EQUAL(Stemmer::stem("привет"), Stemmer::stem_russian("привет"));
    TEST_ASSERT_EQUAL(Stemmer::stem("hello"), Stemmer::stem_english("hello"));
}

void test_stemmer_expand_variants_russian() {
    auto variants = Stemmer::expand_variants("машин");
    TEST_ASSERT(variants.size() > 1);
    bool found = false;
    for (const auto& v : variants) {
        if (v == "машин") { found = true; break; }
    }
    TEST_ASSERT_TRUE(found);
}

void test_stemmer_expand_variants_english() {
    auto variants = Stemmer::expand_variants("run");
    TEST_ASSERT(variants.size() > 1);
    bool found_s = false;
    for (const auto& v : variants) {
        if (v == "runs") { found_s = true; break; }
    }
    TEST_ASSERT_TRUE(found_s);
}

// ============================================================================
// Main
// ============================================================================

int main() {
    // count_tokens_approx tests
    REGISTER_TEST(test_count_tokens_empty);
    REGISTER_TEST(test_count_tokens_english_single_word);
    REGISTER_TEST(test_count_tokens_english_multiple_words);
    REGISTER_TEST(test_count_tokens_english_with_punctuation);
    REGISTER_TEST(test_count_tokens_russian_single_word);
    REGISTER_TEST(test_count_tokens_russian_multiple_words);
    REGISTER_TEST(test_count_tokens_russian_long_sentence);
    REGISTER_TEST(test_count_tokens_mixed_language);

    // split_into_chunks tests
    REGISTER_TEST(test_split_empty_text);
    REGISTER_TEST(test_split_single_sentence);
    REGISTER_TEST(test_split_multiple_sentences_english);
    REGISTER_TEST(test_split_by_exclamation);
    REGISTER_TEST(test_split_by_question);
    REGISTER_TEST(test_split_russian_sentences);
    REGISTER_TEST(test_split_russian_with_dash);
    REGISTER_TEST(test_split_ellipsis_not_boundary);
    REGISTER_TEST(test_split_long_sentence_by_commas);

    // Stemmer tests
    REGISTER_TEST(test_stemmer_is_russian);
    REGISTER_TEST(test_stemmer_is_english);
    REGISTER_TEST(test_stemmer_russian_basic);
    REGISTER_TEST(test_stemmer_russian_short_words);
    REGISTER_TEST(test_stemmer_russian_verbs);
    REGISTER_TEST(test_stemmer_english_basic);
    REGISTER_TEST(test_stemmer_english_short_words);
    REGISTER_TEST(test_stemmer_auto_detect);
    REGISTER_TEST(test_stemmer_expand_variants_russian);
    REGISTER_TEST(test_stemmer_expand_variants_english);

    int result = test::TestRunner::instance().run();
    delete rag;
    return result;
}
