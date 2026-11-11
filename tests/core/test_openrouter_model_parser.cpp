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

/* Ход-вызов: content пустой, tool_calls в сообщении, finish "tool_calls".
 * Раньше парсер не читал message.tool_calls, и фолбэк complete_streaming_async
 * выбрасывал такой ответ целиком: модель вызывала инструмент, а вызывающий
 * получал «пустой» ход. */
void test_parse_completion_response_native_tool_calls() {
    OpenRouterModelParser parser;

    json response_json = {
        {"id", "resp-tool"},
        {"model", "glm-4.5-flash"},
        {"choices", {{
            {"message", {
                {"role", "assistant"},
                {"content", ""},
                {"tool_calls", {{
                    {"id", "call_1"},
                    {"type", "function"},
                    {"index", 0},
                    {"function", {
                        {"name", "read_file"},
                        {"arguments", "{\"path\":\"/tmp/x\"}"}
                    }}
                }}}
            }},
            {"finish_reason", "tool_calls"}
        }}},
        {"usage", {{"prompt_tokens", 30}, {"completion_tokens", 12}, {"total_tokens", 42}}}
    };

    auto response = parser.parse_completion_response(response_json.dump());

    TEST_ASSERT_TRUE(response.success);
    TEST_ASSERT_EQUAL(response.content, "");
    TEST_ASSERT_EQUAL(response.finish_reason, std::string("tool_calls"));
    TEST_ASSERT_TRUE(!response.tool_calls.empty());
    TEST_ASSERT_EQUAL(response.tool_calls.size(), 1u);
    TEST_ASSERT_EQUAL(response.tool_calls[0].name, std::string("read_file"));
    TEST_ASSERT_EQUAL(response.tool_calls[0].arguments, std::string("{\"path\":\"/tmp/x\"}"));
    TEST_ASSERT_EQUAL(response.tool_calls[0].id, std::string("call_1"));
    TEST_ASSERT_TRUE(response.prompt_tokens > 0);
    TEST_ASSERT_TRUE(response.completion_tokens > 0);
}

/* arguments объектом вместо строки JSON — выравнивается в строку, иначе
 * вызывающий разбирал бы два формата для одного поля. */
void test_parse_completion_response_tool_calls_arguments_object() {
    OpenRouterModelParser parser;

    json response_json = {
        {"choices", {{
            {"message", {
                {"content", ""},
                {"tool_calls", {{
                    {"type", "function"},
                    {"function", {
                        {"name", "bash"},
                        {"arguments", {{"cmd", "ls"}}}
                    }}
                }}}
            }},
            {"finish_reason", "tool_calls"}
        }}}
    };

    auto response = parser.parse_completion_response(response_json.dump());

    TEST_ASSERT_TRUE(response.success);
    TEST_ASSERT_TRUE(!response.tool_calls.empty());
    TEST_ASSERT_EQUAL(response.tool_calls[0].name, std::string("bash"));
    TEST_ASSERT_TRUE(!response.tool_calls[0].arguments.empty());
    /* Восстановленный JSON валиден и несёт поле cmd. */
    json reparse = json::parse(response.tool_calls[0].arguments, nullptr, false);
    TEST_ASSERT_TRUE(!reparse.is_discarded());
    TEST_ASSERT_EQUAL(reparse.value("cmd", ""), "ls");
}

/* Размышление провайдера НЕ подставляется вместо content там, где ответ —
 * нативный вызов: иначе рассуждение попадало бы в ход в виде «ответа». */
void test_parse_completion_response_tool_calls_not_overwritten_by_reasoning() {
    OpenRouterModelParser parser;

    json response_json = {
        {"choices", {{
            {"message", {
                {"content", ""},
                {"reasoning_content", "Сначала надо подумать о калькуляторе..."},
                {"tool_calls", {{
                    {"index", 0},
                    {"function", {
                        {"name", "calc_add"},
                        {"arguments", "{\"a\":2,\"b\":2}"}
                    }}
                }}}
            }},
            {"finish_reason", "tool_calls"}
        }}}
    };

    auto response = parser.parse_completion_response(response_json.dump());

    TEST_ASSERT_TRUE(response.success);
    TEST_ASSERT_EQUAL(response.content, "");
    TEST_ASSERT_EQUAL(response.tool_calls.size(), 1u);
    TEST_ASSERT_EQUAL(response.tool_calls[0].name, std::string("calc_add"));
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
    REGISTER_TEST(test_parse_completion_response_native_tool_calls);
    REGISTER_TEST(test_parse_completion_response_tool_calls_arguments_object);
    REGISTER_TEST(test_parse_completion_response_tool_calls_not_overwritten_by_reasoning);

    return test::TestRunner::instance().run();
}
