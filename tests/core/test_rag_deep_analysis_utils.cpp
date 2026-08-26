#include "../../include/core/rag_deep_analysis_utils.h"
#include "../test_framework.h"

using namespace llama_gui::core;
using namespace llama_gui::core::deep_analysis;

void test_format_duration_ms() {
    TEST_ASSERT_EQUAL(format_duration(500), std::string("500ms"));
    TEST_ASSERT_EQUAL(format_duration(999), std::string("999ms"));
}

void test_format_duration_seconds() {
    TEST_ASSERT_EQUAL(format_duration(1000), std::string("1.0s"));
    TEST_ASSERT_EQUAL(format_duration(1500), std::string("1.5s"));
    TEST_ASSERT_EQUAL(format_duration(59999), std::string("60.0s"));
}

void test_format_duration_minutes() {
    TEST_ASSERT_EQUAL(format_duration(60000), std::string("1.0min"));
    TEST_ASSERT_EQUAL(format_duration(90000), std::string("1.5min"));
}

void test_estimate_tokens() {
    TEST_ASSERT_EQUAL(estimate_tokens(""), 0);
    TEST_ASSERT_EQUAL(estimate_tokens("test"), 1);
    TEST_ASSERT_EQUAL(estimate_tokens("test string"), 2);
    TEST_ASSERT_EQUAL(estimate_tokens("12345678"), 2);
}

void test_trim_summary_basic() {
    std::string text = "This is a summary.";
    TEST_ASSERT_EQUAL(trim_summary(text), std::string("This is a summary."));
}

void test_trim_summary_whitespace() {
    std::string text = "  \n  Summary  \n  ";
    TEST_ASSERT_EQUAL(trim_summary(text), std::string("Summary"));
}

void test_trim_summary_marker() {
    std::string text = "Summary content\n\n=== Extra section";
    TEST_ASSERT_EQUAL(trim_summary(text), std::string("Summary content"));
}

void test_trim_summary_empty() {
    TEST_ASSERT_EQUAL(trim_summary(""), std::string(""));
    TEST_ASSERT_EQUAL(trim_summary("   "), std::string(""));
}

void test_create_batches() {
    std::vector<RagChunk> chunks(5);
    for (int i = 0; i < 5; i++) {
        chunks[i].chunk_index = i;
    }

    auto batches = create_batches(chunks, 2);

    TEST_ASSERT_EQUAL(batches.size(), 3u);
    TEST_ASSERT_EQUAL(batches[0].size(), 2u);
    TEST_ASSERT_EQUAL(batches[1].size(), 2u);
    TEST_ASSERT_EQUAL(batches[2].size(), 1u);
}

void test_create_batches_exact() {
    std::vector<RagChunk> chunks(4);
    auto batches = create_batches(chunks, 2);

    TEST_ASSERT_EQUAL(batches.size(), 2u);
    TEST_ASSERT_EQUAL(batches[0].size(), 2u);
    TEST_ASSERT_EQUAL(batches[1].size(), 2u);
}

void test_create_batches_for_strings() {
    std::vector<std::string> items = {"a", "b", "c", "d", "e"};
    auto batches = create_batches_for_strings(items, 3);

    TEST_ASSERT_EQUAL(batches.size(), 2u);
    TEST_ASSERT_EQUAL(batches[0].size(), 3u);
    TEST_ASSERT_EQUAL(batches[1].size(), 2u);
}

int main() {
    REGISTER_TEST(test_format_duration_ms);
    REGISTER_TEST(test_format_duration_seconds);
    REGISTER_TEST(test_format_duration_minutes);
    REGISTER_TEST(test_estimate_tokens);
    REGISTER_TEST(test_trim_summary_basic);
    REGISTER_TEST(test_trim_summary_whitespace);
    REGISTER_TEST(test_trim_summary_marker);
    REGISTER_TEST(test_trim_summary_empty);
    REGISTER_TEST(test_create_batches);
    REGISTER_TEST(test_create_batches_exact);
    REGISTER_TEST(test_create_batches_for_strings);

    return test::TestRunner::instance().run();
}
