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
#include "../../include/core/base64.h"
#include "../../include/core/image_mime.h"

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

/* --- Изображения (мультимодальность) --- */

/* Текстовая реплика уезжает СТРОКОЙ, даже когда рядом есть картинки у
 * ДРУГОЙ реплики.
 *
 * Проверяется именно это, а не «картинка уехала»: соблазн написать
 * сборку «если в запросе есть хоть одно изображение, все реплики
 * становятся массивами» велик, и он сломал бы ровно то, что работало, —
 * текстовые сообщения. Строка и массив в одном поле JSON — разные типы, и
 * половина серверов отвергла бы массив из одной текстовой части.
 */
void test_text_message_stays_a_string_when_another_message_has_images() {
    ChatCompletionRequest req = minimal_request();
    req.messages.emplace_back(MessageRole::User, "посмотри на это");
    ChatMessage with_image(MessageRole::User, "что здесь не так?");
    ImagePart img;
    img.url = "data:image/png;base64,AAAA";
    img.media_type = "image/png";
    with_image.images.push_back(img);
    req.messages.push_back(with_image);

    const json body = build_chat_body(req, false);
    TEST_ASSERT_EQUAL(body["messages"].size(), size_t(3));
    /* Первая реплика (минимальная, из minimal_request) — строка. */
    TEST_ASSERT_TRUE(body["messages"][0]["content"].is_string());
    /* Вторая — тоже строка: картинка не её. */
    TEST_ASSERT_TRUE(body["messages"][1]["content"].is_string());
    /* Третья — массив частей. */
    TEST_ASSERT_TRUE(body["messages"][2]["content"].is_array());
}

/* Реплика с картинкой и текстом уезжает массивом, где текста часть ПЕРВАЯ.
 *
 * Порядок не косметика: провайдеры читают изображение как иллюстрацию к
 * вопросу, а не наоборот, — тот же порядок, что в примерах OpenAI. */
void test_image_and_text_become_a_parts_array_in_openai_shape() {
    ChatCompletionRequest req = minimal_request();
    ChatMessage msg(MessageRole::User, "что здесь не так?");
    ImagePart img;
    img.url = "data:image/png;base64,AAAA";
    img.media_type = "image/png";
    msg.images.push_back(img);
    req.messages.push_back(msg);

    const json body = build_chat_body(req, false);
    const json& parts = body["messages"][1]["content"];
    TEST_ASSERT_TRUE(parts.is_array());
    TEST_ASSERT_EQUAL(parts.size(), size_t(2));
    TEST_ASSERT_EQUAL(parts[0]["type"].get<std::string>(), std::string("text"));
    TEST_ASSERT_EQUAL(parts[0]["text"].get<std::string>(), std::string("что здесь не так?"));
    TEST_ASSERT_EQUAL(parts[1]["type"].get<std::string>(), std::string("image_url"));
    TEST_ASSERT_EQUAL(parts[1]["image_url"]["url"].get<std::string>(),
              std::string("data:image/png;base64,AAAA"));
}

/* Реплика из ОДНОЙ картинки, без текста, — законный формат.
 *
 * Ровно тот случай, на котором всё ломалось: человек присылает скриншот
 * без подписи, и «пустой текст» не должен ни съедать реплику, ни
 * превращаться в `"content": ""` (модель ответила бы «не вижу картинки»).
 */
void test_image_without_text_sends_only_the_image_part() {
    ChatCompletionRequest req = minimal_request();
    ChatMessage msg(MessageRole::User, "");
    ImagePart img;
    img.url = "data:image/jpeg;base64,BBBB";
    msg.images.push_back(img);
    req.messages.push_back(msg);

    const json body = build_chat_body(req, false);
    const json& parts = body["messages"][1]["content"];
    TEST_ASSERT_TRUE(parts.is_array());
    /* Ровно одна часть: пустая текстовая не добавляется. */
    TEST_ASSERT_EQUAL(parts.size(), size_t(1));
    TEST_ASSERT_EQUAL(parts[0]["type"].get<std::string>(), std::string("image_url"));
}

/* Две картинки сохраняют порядок: порядок вложений человек видит в списке
 * под полем ввода, и он же определяет порядок частей в запросе. */
void test_multiple_images_keep_their_order() {
    ChatCompletionRequest req = minimal_request();
    ChatMessage msg(MessageRole::User, "оба скриншота");
    for (const char* u : {"data:image/png;base64,AAA",
                          "data:image/png;base64,BBB"}) {
        ImagePart img;
        img.url = u;
        msg.images.push_back(img);
    }
    req.messages.push_back(msg);

    /* Тело — в ИМЕНОВАННУЮ переменную, а не сразу в ссылку: `build_chat_body`
     * возвращает временный объект, и `const json&` к его подолу — висячая
     * ссылка. Проверка при этом не падала бы предсказуемо: читалась бы
     * освобождённая память, и «2 части вместо 3» — как раз её случайный
     * отсвет. Тест, который ловит не то, что написано в его имени, хуже
     * отсутствующего. */
    const json body = build_chat_body(req, false);
    const json& parts = body["messages"][1]["content"];
    TEST_ASSERT_EQUAL(parts.size(), size_t(3));
    TEST_ASSERT_TRUE(parts[1]["image_url"]["url"].get<std::string>().find("AAA") !=
                std::string::npos);
    TEST_ASSERT_TRUE(parts[2]["image_url"]["url"].get<std::string>().find("BBB") !=
                std::string::npos);
}

/* Запрос без картинок вообще не меняет форму: это регрессионный страж на
 * «сломали текстовый путь, доехав до мультимодальности». */
void test_request_without_images_is_byte_identical_to_before() {
    ChatCompletionRequest req = minimal_request();
    req.tools.push_back(probe_tool());

    /* Тело с картинками и без них сравнивать нельзя (они разные по
     * смыслу), поэтому проверяется отсутствие массивов: ни одна реплика
     * не должна стать массивом сама по себе. */
    const json body = build_chat_body(req, false);
    for (const auto& m : body["messages"]) {
        TEST_ASSERT_TRUE(m["content"].is_string());
    }
}

/* --- base64 --- */

/* Round-trip на всех трёх длинах хвоста: 3k, 3k+1, 3k+2.
 *
 * Хвост — единственное место, где кодец может ошибиться молча: тройки
 * байтов кодируются одинаково при любой длине, а вот остаток 1 или 2
 * требует «=» и сдвигов, и ошибка там даёт не отказ, а ИСПОРЧЕННЫЕ
 * байты — то есть картинку, которую модель «увидела» неправильно.
 */
void test_base64_round_trips_every_tail_length() {
    for (size_t n = 0; n <= 9; ++n) {
        std::string in;
        /* Байты, а не символы: 0x00 и 0xFF обязаны переживать кодирование,
         * иначе «текстовый» тест был бы зелёным на двоичных данных. */
        for (size_t i = 0; i < n; ++i) {
            in += static_cast<char>((i * 37 + 11) & 0xFF);
        }
        std::string dec;
        TEST_ASSERT_TRUE(base64_decode(base64_encode(in), dec));
        TEST_ASSERT_EQUAL(dec.size(), in.size());
        TEST_ASSERT_TRUE(dec == in);
    }
}

/* Известная строка из RFC 4648: "hello" → "aGVsbG8=".
 *
 * Проверка не «сам с собою», а с внешним эталоном: round-trip был бы
 * зелёным и на кодеке, который кодирует в ROT13, а сам с собой
 * декодирует обратно.
 */
void test_base64_matches_a_known_vector() {
    TEST_ASSERT_EQUAL(base64_encode("hello"), std::string("aGVsbG8="));
    TEST_ASSERT_EQUAL(base64_encode(""), std::string(""));
    TEST_ASSERT_EQUAL(base64_encode("f"), std::string("Zg=="));
    TEST_ASSERT_EQUAL(base64_encode("fo"), std::string("Zm8="));
}

/* Мусор в base64 — отказ, а не «разобрали как получилось».
 *
 * Тело приходит от посторонних (вложение), и «достать что-нибудь» здесь
 * означало бы отдать модели произвольные байты. */
void test_base64_rejects_garbage() {
    std::string out;
    TEST_ASSERT_FALSE(base64_decode("!!!!", out));
    TEST_ASSERT_FALSE(base64_decode("abc", out));   /* обрезанная quadruple */
    TEST_ASSERT_FALSE(base64_decode("ab=c", out));  /* «=» в середине */
}

/* --- Определение типа по содержимому --- */

/* Тип берётся из МАГИЧЕСКИХ БАЙТОВ, а не из расширения: файл `.png`, внутри
 * которого HTML, — это не картинка, и поверив расширению, мы отдали бы
 * модели base64 от страницы с `"type":"image_url"`. */
void test_image_mime_is_detected_from_content_not_extension() {
    TEST_ASSERT_EQUAL(detect_image_mime(std::string("\x89PNG\r\n\x1A\n", 8)),
              std::string("image/png"));
    TEST_ASSERT_EQUAL(detect_image_mime(std::string("\xFF\xD8\xFF\xE0", 4)),
              std::string("image/jpeg"));
    TEST_ASSERT_EQUAL(detect_image_mime(std::string("GIF89a......", 12)),
              std::string("image/gif"));
    TEST_ASSERT_EQUAL(detect_image_mime(std::string("RIFF\x00\x00\x00\x00WEBP", 16)),
              std::string("image/webp"));

    /* HTML и текст — НЕ картинки, как бы они ни назывались. */
    TEST_ASSERT_TRUE(detect_image_mime("<!DOCTYPE html><html></html>").empty());
    TEST_ASSERT_TRUE(detect_image_mime("{\"a\": 1}").empty());
    TEST_ASSERT_TRUE(detect_image_mime("").empty());
    /* Обрезанный заголовок (меньше сигнатуры) — не картинка. */
    TEST_ASSERT_TRUE(detect_image_mime(std::string("\x89PNG", 4)).empty());
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
    REGISTER_TEST(test_text_message_stays_a_string_when_another_message_has_images);
    REGISTER_TEST(test_image_and_text_become_a_parts_array_in_openai_shape);
    REGISTER_TEST(test_image_without_text_sends_only_the_image_part);
    REGISTER_TEST(test_multiple_images_keep_their_order);
    REGISTER_TEST(test_request_without_images_is_byte_identical_to_before);
    REGISTER_TEST(test_base64_round_trips_every_tail_length);
    REGISTER_TEST(test_base64_matches_a_known_vector);
    REGISTER_TEST(test_base64_rejects_garbage);
    REGISTER_TEST(test_image_mime_is_detected_from_content_not_extension);

    return test::TestRunner::instance().run();
}
