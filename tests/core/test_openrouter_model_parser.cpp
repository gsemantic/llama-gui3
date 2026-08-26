#include "../../include/core/openrouter_model_parser.h"
#include "../test_framework.h"
#include <nlohmann/json.hpp>

using namespace llama_gui::core;
using json = nlohmann::json;

void test_parse_model_basic() {
    OpenRouterModelParser parser;

    json model_json = {
        {"id", "meta-llama/llama-3-8b-instruct"},
        {"name", "Llama 3 8B"},
        {"description", "A test model"},
        {"context_length", 8192}
    };

    auto model = parser.parse_model(model_json);

    TEST_ASSERT_EQUAL(model.id, std::string("meta-llama/llama-3-8b-instruct"));
    TEST_ASSERT_EQUAL(model.name, std::string("Llama 3 8B"));
    TEST_ASSERT_EQUAL(model.description, std::string("A test model"));
    TEST_ASSERT_EQUAL(model.context_length, 8192);
}

void test_parse_model_free_detection() {
    OpenRouterModelParser parser;

    json model_json = {
        {"id", "free-model"},
        {"name", "Free Model"},
        {"pricing", {{"prompt", "0"}, {"completion", "0"}}}
    };

    auto model = parser.parse_model(model_json);
    TEST_ASSERT_TRUE(model.is_free);

    json paid_model_json = {
        {"id", "paid-model"},
        {"name", "Paid Model"},
        {"pricing", {{"prompt", "0.001"}, {"completion", "0.002"}}}
    };

    auto paid_model = parser.parse_model(paid_model_json);
    TEST_ASSERT_FALSE(paid_model.is_free);
}

void test_parse_model_provider() {
    OpenRouterModelParser parser;

    json model_json = {
        {"id", "test-model"},
        {"name", "Test Model"},
        {"provider", {{"name", "OpenAI"}}}
    };

    auto model = parser.parse_model(model_json);
    TEST_ASSERT_EQUAL(model.provider, std::string("OpenAI"));
}

void test_parse_models_response_valid() {
    OpenRouterModelParser parser;

    json response_json = {
        {"data", {
            {{"id", "model-1"}, {"name", "Model 1"}},
            {{"id", "model-2"}, {"name", "Model 2"}}
        }}
    };

    auto response = parser.parse_models_response(response_json.dump());

    TEST_ASSERT_TRUE(response.success);
    TEST_ASSERT_EQUAL(response.models.size(), 2u);
}

void test_parse_models_response_invalid() {
    OpenRouterModelParser parser;

    auto response = parser.parse_models_response("not json");
    TEST_ASSERT_FALSE(response.success);
    TEST_ASSERT_FALSE(response.error.empty());
}

void test_filter_models_query() {
    OpenRouterModelParser parser;

    std::vector<OpenRouterModel> models = {
        {"meta-llama/llama-3-8b", "Llama 3 8B", "Meta", ""},
        {"openai/gpt-4", "GPT-4", "OpenAI", ""},
        {"google/gemini-pro", "Gemini Pro", "Google", ""}
    };

    auto filtered = parser.filter_models(models, "llama", false);
    TEST_ASSERT_EQUAL(filtered.size(), 1u);
    TEST_ASSERT_EQUAL(filtered[0].id, std::string("meta-llama/llama-3-8b"));
}

void test_filter_models_free_only() {
    OpenRouterModelParser parser;

    std::vector<OpenRouterModel> models = {
        {"free-model", "Free Model", "Provider", "", 0, true},
        {"paid-model", "Paid Model", "Provider", "", 0, false}
    };

    auto filtered = parser.filter_models(models, "", true);
    TEST_ASSERT_EQUAL(filtered.size(), 1u);
    TEST_ASSERT_EQUAL(filtered[0].id, std::string("free-model"));
}

void test_parse_completion_response_success() {
    OpenRouterModelParser parser;

    json response_json = {
        {"id", "resp-123"},
        {"model", "llama-3-8b"},
        {"choices", {{
            {"message", {{"content", "Hello world"}}},
            {"finish_reason", "stop"}
        }}},
        {"usage", {{"prompt_tokens", 10}, {"completion_tokens", 5}, {"total_tokens", 15}}}
    };

    auto response = parser.parse_completion_response(response_json.dump());

    TEST_ASSERT_TRUE(response.success);
    TEST_ASSERT_EQUAL(response.content, std::string("Hello world"));
    TEST_ASSERT_EQUAL(response.prompt_tokens, 10);
    TEST_ASSERT_EQUAL(response.completion_tokens, 5);
}

void test_parse_completion_response_error_429() {
    OpenRouterModelParser parser;

    json error_json = {
        {"error", {
            {"message", "Rate limit exceeded"},
            {"code", 429}
        }}
    };

    auto response = parser.parse_completion_response(error_json.dump());

    TEST_ASSERT_FALSE(response.success);
    TEST_ASSERT_FALSE(response.error.empty());
}

void test_parse_completion_response_error_401() {
    OpenRouterModelParser parser;

    json error_json = {
        {"error", {
            {"message", "Unauthorized"},
            {"code", 401}
        }}
    };

    auto response = parser.parse_completion_response(error_json.dump());

    TEST_ASSERT_FALSE(response.success);
    TEST_ASSERT_FALSE(response.error.empty());
}

int main() {
    REGISTER_TEST(test_parse_model_basic);
    REGISTER_TEST(test_parse_model_free_detection);
    REGISTER_TEST(test_parse_model_provider);
    REGISTER_TEST(test_parse_models_response_valid);
    REGISTER_TEST(test_parse_models_response_invalid);
    REGISTER_TEST(test_filter_models_query);
    REGISTER_TEST(test_filter_models_free_only);
    REGISTER_TEST(test_parse_completion_response_success);
    REGISTER_TEST(test_parse_completion_response_error_429);
    REGISTER_TEST(test_parse_completion_response_error_401);

    return test::TestRunner::instance().run();
}
