#include "../include/core/context_monitor.h"
#include "test_framework.h"
#include <string>
#include <vector>

using namespace llama_gui::core;

// Тест: подсчёт токенов
void test_estimate_tokens_empty() {
    TEST_ASSERT_EQUAL(ContextMonitor::estimate_tokens(""), 0);
}

void test_estimate_tokens_english() {
    int tokens = ContextMonitor::estimate_tokens("Hello world");
    TEST_ASSERT(tokens >= 1 && tokens <= 5);
}

void test_estimate_tokens_russian() {
    int tokens = ContextMonitor::estimate_tokens("Привет мир");
    TEST_ASSERT(tokens >= 1 && tokens <= 5);
}

// Тест: обновление статистики
void test_update_basic() {
    ContextMonitor monitor;

    std::vector<std::string> messages = {
        "Hello",
        "World",
        "Test message"
    };

    monitor.update(messages, "", "", 4096);
    auto stats = monitor.get_stats();

    TEST_ASSERT(stats.used_tokens > 0);
    TEST_ASSERT_EQUAL(stats.max_tokens, 4096);
    TEST_ASSERT(stats.available_tokens > 0);
}

// Тест: определение уровня использования
void test_usage_levels() {
    ContextMonitor monitor;

    // Мало сообщений - Normal
    std::vector<std::string> few_messages = {"Hi"};
    monitor.update(few_messages, "", "", 4096);
    TEST_ASSERT_EQUAL(static_cast<int>(monitor.get_stats().level), static_cast<int>(ContextUsageLevel::Normal));

    // Очень много длинных сообщений с маленьким контекстом - должен быть Warning или выше
    std::vector<std::string> many_messages;
    for (int i = 0; i < 200; ++i) {
        many_messages.push_back("This is a very long test message with lots of content to fill up the context window quickly");
    }
    monitor.update(many_messages, "", "", 1024); // Маленький контекст для теста
    TEST_ASSERT(monitor.get_stats().level >= ContextUsageLevel::Warning);
}

// Тест: необходимость сжатия
void test_needs_compression() {
    ContextMonitor monitor;

    // Мало сообщений - сжатие не нужно
    std::vector<std::string> few_messages = {"Hi"};
    monitor.update(few_messages, "", "", 4096);
    TEST_ASSERT_FALSE(monitor.needs_compression());

    // Много длинных сообщений с маленьким контекстом - сжатие нужно
    std::vector<std::string> many_messages;
    for (int i = 0; i < 200; ++i) {
        many_messages.push_back("Very long message to fill up context quickly with lots of content");
    }
    monitor.update(many_messages, "", "", 512); // Очень маленький контекст
    TEST_ASSERT(monitor.needs_compression());
}

// Тест: количество сообщений для суммаризации
void test_messages_to_summarize() {
    ContextMonitor monitor;

    // Когда не нужно сжатие - 0 сообщений
    std::vector<std::string> few_messages = {"Hi"};
    monitor.update(few_messages, "", "", 4096);
    TEST_ASSERT_EQUAL(monitor.get_messages_to_summarize(), 0);
}

// Тест: callback при изменении порога
void test_threshold_callback() {
    ContextMonitor monitor;
    bool callback_called = false;

    monitor.set_threshold_callback([&](ContextUsageLevel, const ContextStats&) {
        callback_called = true;
    });

    // Много сообщений для триггера callback
    std::vector<std::string> many_messages;
    for (int i = 0; i < 200; ++i) {
        many_messages.push_back("Fill up context");
    }

    monitor.update(many_messages, "", "", 4096);
    // Callback может быть вызван если уровень изменился
    // В этом тесте просто проверяем что не крашнулось
}

// Тест: учёт системного промпта
void test_system_prompt_accounted() {
    ContextMonitor monitor;

    std::vector<std::string> messages = {"Hello"};
    std::string system_prompt = "You are a helpful assistant with access to external documents.";

    monitor.update(messages, system_prompt, "", 4096);
    auto stats_with_prompt = monitor.get_stats();

    monitor.update(messages, "", "", 4096);
    auto stats_without_prompt = monitor.get_stats();

    TEST_ASSERT(stats_with_prompt.used_tokens > stats_without_prompt.used_tokens);
}

// Тест: учёт RAG контекста
void test_rag_context_accounted() {
    ContextMonitor monitor;

    std::vector<std::string> messages = {"Hello"};
    std::string rag_context = "Here is some relevant context from documents: ...";

    monitor.update(messages, "", rag_context, 4096);
    auto stats_with_rag = monitor.get_stats();

    monitor.update(messages, "", "", 4096);
    auto stats_without_rag = monitor.get_stats();

    TEST_ASSERT(stats_with_rag.used_tokens > stats_without_rag.used_tokens);
}

// Тест: процент использования
void test_usage_percent() {
    ContextMonitor monitor;

    std::vector<std::string> messages = {"Hello"};
    monitor.update(messages, "", "", 4096);

    double percent = monitor.get_stats().usage_percent();
    TEST_ASSERT(percent >= 0.0 && percent <= 100.0);
}

// Тест: available_tokens
void test_available_tokens() {
    ContextMonitor monitor;

    std::vector<std::string> messages = {"Hello"};
    monitor.update(messages, "", "", 4096);

    auto stats = monitor.get_stats();
    TEST_ASSERT_EQUAL(stats.available_tokens, stats.max_tokens - stats.used_tokens);
}

int main() {
    REGISTER_TEST(test_estimate_tokens_empty);
    REGISTER_TEST(test_estimate_tokens_english);
    REGISTER_TEST(test_estimate_tokens_russian);
    REGISTER_TEST(test_update_basic);
    REGISTER_TEST(test_usage_levels);
    REGISTER_TEST(test_needs_compression);
    REGISTER_TEST(test_messages_to_summarize);
    REGISTER_TEST(test_threshold_callback);
    REGISTER_TEST(test_system_prompt_accounted);
    REGISTER_TEST(test_rag_context_accounted);
    REGISTER_TEST(test_usage_percent);
    REGISTER_TEST(test_available_tokens);

    return test::TestRunner::instance().run();
}
