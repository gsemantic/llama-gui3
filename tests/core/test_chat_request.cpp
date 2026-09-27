/*
 * test_chat_request.cpp — тело запроса к /v1/chat/completions и вывод о
 * поддержке tools (И6.2).
 *
 * Обе функции чистые: ни сети, ни состояния. Проверяется ровно то, что
 * может испортиться молча, — состав тела запроса и решение об отказе.
 *
 * Главное свойство набора: поле, которое не наполнили, в тело не уходит.
 * Если бы tools уезжали всегда, каждый запрос без инструментов рисковал бы
 * получить 400 у сервера без поддержки — то есть отказ пришёл бы не туда,
 * где он вызван.
 */

#include "../../include/core/llama_interface.h"

#include "../test_framework.h"

#include <nlohmann/json.hpp>

using namespace llama_gui::core;
using json = nlohmann::json;

namespace {

ChatCompletionRequest minimal_request() {
    ChatCompletionRequest req;
    req.model = "local";
    req.messages.emplace_back(MessageRole::User, "привет");
    /* Нейтральные значения: запрос без сознательно заданного сэмплирования. */
    req.top_k = 0;
    req.min_p = 0.0f;
    req.repeat_penalty = 1.0f;
    return req;
}

ToolSpec probe_tool() {
    ToolSpec t;
    t.name = "read_file";
    t.description = "читает файл";
    t.parameters = json{{"type", "object"},
                        {"properties", json{{"path", json{{"type", "string"}}}}},
                        {"required", json::array({"path"})}};
    return t;
}

} // namespace

/* --- Состав тела --- */

/* Запрос без tools не несёт ни tools, ни tool_choice: иначе сервер без
 * поддержки отказал бы на пустом месте, и отказ выглядел бы как ошибка
 * агента, а не как отсутствие возможности. */
void test_no_tools_means_no_tools_fields() {
    const json body = build_chat_body(minimal_request(), false);
    TEST_ASSERT_FALSE(body.contains("tools"));
    TEST_ASSERT_FALSE(body.contains("tool_choice"));
    TEST_ASSERT_EQUAL(body["stream"].get<bool>(), false);
    TEST_ASSERT_EQUAL(body["messages"].size(), size_t(1));
    TEST_ASSERT_EQUAL(body["messages"][0]["role"].get<std::string>(),
                      std::string("user"));
}

/* С tools уходят и сам список, и выбор режима, и схема аргументов. */
void test_tools_are_serialized_in_openai_shape() {
    ChatCompletionRequest req = minimal_request();
    req.tools.push_back(probe_tool());
    req.tool_choice = "auto";

    const json body = build_chat_body(req, true);
    TEST_ASSERT_EQUAL(body["tools"].size(), size_t(1));
    TEST_ASSERT_EQUAL(body["tools"][0]["type"].get<std::string>(),
                      std::string("function"));
    TEST_ASSERT_EQUAL(body["tools"][0]["function"]["name"].get<std::string>(),
                      std::string("read_file"));
    TEST_ASSERT_EQUAL(body["tool_choice"].get<std::string>(), std::string("auto"));
    /* Схема обязана доехать: без неё сервер не знает, что проверять, и
     * придумает аргументы сам. */
    TEST_ASSERT_EQUAL(
        body["tools"][0]["function"]["parameters"]["required"][0].get<std::string>(),
        std::string("path"));
}

/* tool_choice без tools не уходит: сервер трактует его как просьбу вызвать
 * инструмент, которого не объявлено, и отвечает 400. */
void test_tool_choice_without_tools_is_not_sent() {
    ChatCompletionRequest req = minimal_request();
    req.tool_choice = "required";
    const json body = build_chat_body(req, false);
    TEST_ASSERT_FALSE(body.contains("tool_choice"));
}

/* Пустой tool_choice не уходит даже вместе с tools: пустая строка — не
 * значение «выбор режима», и сервер отвечает на неё ошибкой разбора. */
void test_empty_tool_choice_is_not_sent_with_tools() {
    ChatCompletionRequest req = minimal_request();
    req.tools.push_back(probe_tool());
    const json body = build_chat_body(req, false);
    TEST_ASSERT(body.contains("tools"));
    TEST_ASSERT_FALSE(body.contains("tool_choice"));
}

/* Вид инструмента и вызова доезжают в обе стороны: сервер отвергает вид,
 * которого не знает («Unsupported tool type»), и подмена его на
 * «function» маскировала бы это как рабочий вызов. */
void test_tool_kind_travels_in_both_directions() {
    ChatCompletionRequest req = minimal_request();
    ToolSpec t = probe_tool();
    t.type = "custom";
    req.tools.push_back(t);
    TEST_ASSERT_EQUAL(build_chat_body(req, false)["tools"][0]["type"].get<std::string>(),
                      std::string("custom"));

    const json calls = json::array({
        json{{"id", "call_0"}, {"type", "custom"},
             {"function", {{"name", "ping"}, {"arguments", "{}"}}}}});
    TEST_ASSERT_EQUAL(parse_tool_calls(calls)[0].type, std::string("custom"));
}

/* Вид по умолчанию — function, и он проставляется, а не остаётся пустым. */
void test_tool_kind_defaults_to_function() {
    ChatCompletionRequest req = minimal_request();
    req.tools.push_back(probe_tool());
    TEST_ASSERT_EQUAL(build_chat_body(req, false)["tools"][0]["type"].get<std::string>(),
                      std::string("function"));
}

/* Инструмент без схемы объявляется с пустой объектной схемой, а не без
 * parameters: у сервера нет значения по умолчанию, и отсутствие поля
 * трактуется как ошибка формата. */
void test_tool_without_parameters_gets_an_empty_object_schema() {
    ChatCompletionRequest req = minimal_request();
    ToolSpec bare;
    bare.name = "ping";
    req.tools.push_back(bare);

    const json body = build_chat_body(req, false);
    TEST_ASSERT_EQUAL(body["tools"][0]["function"]["parameters"]["type"].get<std::string>(),
                      std::string("object"));
    TEST_ASSERT(body["tools"][0]["function"]["parameters"]["properties"].is_object());
    /* Описание пустое — поле не выдумывается. */
    TEST_ASSERT_FALSE(body["tools"][0]["function"].contains("description"));
}

/* Ответ ограничивается только тогда, когда это сказано. */
void test_response_format_is_optional_and_typed() {
    ChatCompletionRequest req = minimal_request();
    TEST_ASSERT_FALSE(build_chat_body(req, false).contains("response_format"));

    req.response_format.type = "json_object";
    TEST_ASSERT_EQUAL(
        build_chat_body(req, false)["response_format"]["type"].get<std::string>(),
        std::string("json_object"));

    req.response_format.type = "json_schema";
    req.response_format.schema = json{{"name", "answer"}, {"schema", json::object()}};
    const json with_schema = build_chat_body(req, false)["response_format"];
    TEST_ASSERT_EQUAL(with_schema["json_schema"]["name"].get<std::string>(),
                      std::string("answer"));
}

/* Нейтральное сэмплирование в тело не уходит: «здесь нужны настройки
 * сервера» должно говориться явно, а не молчанием поля. */
void test_neutral_sampling_params_are_not_sent() {
    const json body = build_chat_body(minimal_request(), false);
    TEST_ASSERT_FALSE(body.contains("top_k"));
    TEST_ASSERT_FALSE(body.contains("min_p"));
    TEST_ASSERT_FALSE(body.contains("repeat_penalty"));
    TEST_ASSERT_FALSE(body.contains("mirostat"));
    TEST_ASSERT_FALSE(body.contains("stop"));
}

void test_non_neutral_sampling_params_are_sent() {
    ChatCompletionRequest req = minimal_request();
    req.top_k = 40;
    req.min_p = 0.05f;
    req.repeat_penalty = 1.1f;
    req.mirostat_mode = 2;
    req.stop.push_back("STOP");

    const json body = build_chat_body(req, true);
    TEST_ASSERT_EQUAL(body["top_k"].get<int>(), 40);
    TEST_ASSERT_EQUAL(body["min_p"].get<float>(), 0.05f);
    TEST_ASSERT_EQUAL(body["repeat_penalty"].get<float>(), 1.1f);
    TEST_ASSERT_EQUAL(body["mirostat"].get<int>(), 2);
    TEST_ASSERT_EQUAL(body["stop"].size(), size_t(1));
}

/* Реплика ассистента с вызовом уезжает с вызовом: без него модель не видит
 * на следующем шаге, что инструмент уже вызывался, и вызывает снова. */
void test_assistant_tool_call_travels_back_to_the_model() {
    ChatCompletionRequest req = minimal_request();
    ChatMessage reply(MessageRole::Assistant, "");
    ToolCall call;
    call.id = "call_0";
    call.name = "read_file";
    call.arguments = "{\"path\":\"a.txt\"}";
    reply.tool_calls.push_back(call);
    req.messages.push_back(reply);

    const json body = build_chat_body(req, false);
    TEST_ASSERT_EQUAL(body["messages"].size(), size_t(2));
    const json& out = body["messages"][1];
    TEST_ASSERT_EQUAL(out["role"].get<std::string>(), std::string("assistant"));
    TEST_ASSERT_EQUAL(out["tool_calls"].size(), size_t(1));
    TEST_ASSERT_EQUAL(out["tool_calls"][0]["function"]["name"].get<std::string>(),
                      std::string("read_file"));
}

/* Оба пути собирают тело одним и тем же сборщиком: единственная проверка
 * против дрейфа — сравнить их на одном и том же запросе. */
void test_streaming_and_blocking_bodies_differ_only_in_stream_flag() {
    ChatCompletionRequest req = minimal_request();
    req.tools.push_back(probe_tool());
    json a = build_chat_body(req, true);
    json b = build_chat_body(req, false);
    TEST_ASSERT_EQUAL(a["stream"].get<bool>(), true);
    TEST_ASSERT_EQUAL(b["stream"].get<bool>(), false);
    a.erase("stream");
    b.erase("stream");
    TEST_ASSERT_EQUAL(a, b);
}

/* --- Вывод о поддержке tools --- */

/* 2xx = сервер принял. */
void test_success_means_supported() {
    TEST_ASSERT(classify_tools_response(200, "") == ToolsSupport::Supported);
    TEST_ASSERT(classify_tools_response(201, "{\"choices\":[]}") == ToolsSupport::Supported);
}

/* Сетевой сбой — НЕ «не умеет». Это правило держит весь feature-detect:
 * запомнив отказ по сети, мы одним сбоем выключили бы нативный вызов
 * навсегда. Вторая половина проверки — с телом, где про tools написано:
 * без неё гвард «нет ответа» был бы избыточным, потому что пустое тело и
 * так даёт Unknown, и такая проверка не могла бы упасть. */
void test_network_failure_is_not_a_rejection() {
    TEST_ASSERT(classify_tools_response(0, "") == ToolsSupport::Unknown);
    TEST_ASSERT(classify_tools_response(0, "connection refused") == ToolsSupport::Unknown);
    /* Ответ не получен — значит, сервер ничего не сказал. Тело, которое мы
     * успели накопить (или ошибочно приняли за ответ), не должно
     * превращать сбой связи в доказательство неумения. */
    TEST_ASSERT(classify_tools_response(0, "tools param requires --jinja flag") ==
                ToolsSupport::Unknown);
}

/* Явный отказ сервера про tools — отказ. Тело проверяется на слова про
 * tools, потому что 400 бывает и о другом (например, «контекст не влез»),
 * и такой 400 не должен выключать tools навсегда. */
void test_explicit_rejection_is_recognized() {
    TEST_ASSERT(classify_tools_response(
                    400, "{\"error\":{\"message\":\"tools param requires --jinja flag\"}}") ==
                ToolsSupport::Rejected);
    TEST_ASSERT(classify_tools_response(400, "Invalid tool_choice: bogus") ==
                ToolsSupport::Rejected);
    TEST_ASSERT(classify_tools_response(422, "Unsupported tool type: custom") ==
                ToolsSupport::Rejected);
}

/* Регистр не должен решать: сообщение сервера может прийти в любом. */
void test_rejection_marker_is_case_insensitive() {
    TEST_ASSERT(classify_tools_response(400, "TOOLS PARAM REQUIRES --JINJA") ==
                ToolsSupport::Rejected);
}

/* 400 не про tools остаётся неизвестным. */
void test_unrelated_bad_request_is_not_a_rejection() {
    TEST_ASSERT(classify_tools_response(400, "{\"error\":{\"message\":\"context shift is disabled\"}}") ==
                ToolsSupport::Unknown);
    /* 5xx — сервер упал, а не отказал по возможностям. */
    TEST_ASSERT(classify_tools_response(500, "tools param requires --jinja") == ToolsSupport::Unknown);
}

/* --- Разбор вызовов из ответа --- */

/* Аргументы остаются сырыми: на последнем чанке провайдера они бывают
 * неполными, и разбор здесь означал бы, что неполный вызов потерян. */
void test_tool_call_arguments_stay_raw() {
    const json calls = json::array({
        json{{"id", "call_0"},
             {"type", "function"},
             {"function", {{"name", "read_file"}, {"arguments", "{\"path\":"}}}}});
    const std::vector<ToolCall> parsed = parse_tool_calls(calls);
    TEST_ASSERT_EQUAL(parsed.size(), size_t(1));
    TEST_ASSERT_EQUAL(parsed[0].id, std::string("call_0"));
    TEST_ASSERT_EQUAL(parsed[0].name, std::string("read_file"));
    TEST_ASSERT_EQUAL(parsed[0].arguments, std::string("{\"path\":"));
}

/* Два вызова не сливаются: порядок в массиве сохраняется, даже когда
 * идентификаторов нет (провайдер нумерует индексом — находка И5.1). */
void test_calls_without_ids_keep_their_order() {
    const json calls = json::array({
        json{{"index", 0}, {"function", {{"name", "read_file"}, {"arguments", "{}"}}}},
        json{{"index", 1}, {"function", {{"name", "grep"}, {"arguments", "{}"}}}}});
    const std::vector<ToolCall> parsed = parse_tool_calls(calls);
    TEST_ASSERT_EQUAL(parsed.size(), size_t(2));
    TEST_ASSERT_EQUAL(parsed[0].name, std::string("read_file"));
    TEST_ASSERT_EQUAL(parsed[1].name, std::string("grep"));
    /* Идентификатора нет — и выдумывать его здесь нельзя: настоящий
     * придёт с последующим чанком, и выдуманный связался бы не с тем. */
    TEST_ASSERT_EQUAL(parsed[0].id, std::string(""));
}

/* Вызов без имени исполнить нечем: пропустив его молча, мы оставили бы в
 * ответе запись, которую цикл счёл бы выполненным. */
void test_call_without_name_is_dropped() {
    const json calls = json::array({
        json{{"id", "call_0"}, {"function", {{"arguments", "{}"}}}},
        json{{"id", "call_1"}, {"function", {{"name", "grep"}, {"arguments", "{}"}}}}});
    const std::vector<ToolCall> parsed = parse_tool_calls(calls);
    TEST_ASSERT_EQUAL(parsed.size(), size_t(1));
    TEST_ASSERT_EQUAL(parsed[0].name, std::string("grep"));
}

/* Мусор во входе не роняет разбор: провайдер может прислать не-массив или
 * не-объект, и ход не должен из-за этого теряться.
 *
 * Чего эта проверка НЕ ловит: сам гвард `if (!array.is_array())`. nlohmann
 * итерирует не-контейнер как одно значение, и `!c.is_object()` отбрасывает
 * и его, поэтому гвард и без него даёт тот же результат. Он оставлен не
 * как проверка, а чтобы не держаться на таком поведении библиотеки. */
void test_garbage_input_yields_no_calls() {
    TEST_ASSERT(parse_tool_calls(json("not an array")).empty());
    TEST_ASSERT(parse_tool_calls(json::array({"text", 42})).empty());
    TEST_ASSERT(parse_tool_calls(json::object()).empty());
}

int main() {
    REGISTER_TEST(test_no_tools_means_no_tools_fields);
    REGISTER_TEST(test_tools_are_serialized_in_openai_shape);
    REGISTER_TEST(test_tool_choice_without_tools_is_not_sent);
    REGISTER_TEST(test_empty_tool_choice_is_not_sent_with_tools);
    REGISTER_TEST(test_tool_kind_travels_in_both_directions);
    REGISTER_TEST(test_tool_kind_defaults_to_function);
    REGISTER_TEST(test_tool_without_parameters_gets_an_empty_object_schema);
    REGISTER_TEST(test_response_format_is_optional_and_typed);
    REGISTER_TEST(test_neutral_sampling_params_are_not_sent);
    REGISTER_TEST(test_non_neutral_sampling_params_are_sent);
    REGISTER_TEST(test_assistant_tool_call_travels_back_to_the_model);
    REGISTER_TEST(test_streaming_and_blocking_bodies_differ_only_in_stream_flag);
    REGISTER_TEST(test_success_means_supported);
    REGISTER_TEST(test_network_failure_is_not_a_rejection);
    REGISTER_TEST(test_explicit_rejection_is_recognized);
    REGISTER_TEST(test_rejection_marker_is_case_insensitive);
    REGISTER_TEST(test_unrelated_bad_request_is_not_a_rejection);
    REGISTER_TEST(test_tool_call_arguments_stay_raw);
    REGISTER_TEST(test_calls_without_ids_keep_their_order);
    REGISTER_TEST(test_call_without_name_is_dropped);
    REGISTER_TEST(test_garbage_input_yields_no_calls);

    return test::TestRunner::instance().run();
}
