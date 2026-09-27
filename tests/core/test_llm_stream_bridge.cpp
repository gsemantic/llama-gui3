/*
 * test_llm_stream_bridge.cpp — раскладка чанков провайдера на колбэки
 * плагина (И6.1).
 *
 * Проверяется без сети и без PluginManager: мост чистый, поэтому единственное,
 * что здесь можно испортить, — раскладка чанка, а её и проверяем.
 *
 * Число тестов не цель, а вот какие свойства обязаны падать при своей
 * поломке — перечислены в комментариях к тестам: они отбирались по типу
 * дефекта, который мост способен допустить (потерянная дельта, слипшееся
 * размышление, два вызова в один, оборванный поток как успех).
 */

#include "../../src/plugins/llm_stream_bridge.h"
#include "../../include/plugins/plugin_api.h"  // LLAMA_STREAM_DELTA_*
#include "../test_framework.h"

#include <nlohmann/json.hpp>

using namespace llama_gui::plugin;
using json = nlohmann::json;

namespace {

/* Приёмник колбэков: пишет всё, что пришло, чтобы тест мог посмотреть на
 * последовательность целиком, а не на последний вызов. */
struct Recorder {
    struct TextPiece {
        std::string text;
        int kind;
    };
    struct ToolPiece {
        std::string call_id;
        std::string tool_name;
        std::string fragment;
    };

    std::vector<TextPiece> texts;
    std::vector<ToolPiece> tools;

    static void on_delta(void* user_data, const char* text, int kind) {
        auto* self = static_cast<Recorder*>(user_data);
        self->texts.push_back({text ? text : "", kind});
    }
    static void on_tool_delta(void* user_data, const char* call_id,
                              const char* tool_name, const char* fragment) {
        auto* self = static_cast<Recorder*>(user_data);
        self->tools.push_back({call_id ? call_id : "", tool_name ? tool_name : "",
                               fragment ? fragment : ""});
    }

    std::string text() const {
        std::string out;
        for (const auto& p : texts) {
            if (p.kind == LLAMA_STREAM_DELTA_TEXT) out += p.text;
        }
        return out;
    }
    std::string reasoning() const {
        std::string out;
        for (const auto& p : texts) {
            if (p.kind == LLAMA_STREAM_DELTA_REASONING) out += p.text;
        }
        return out;
    }
};

/* Подать чанк в мост. Имя нужно, а не .c_str() на временном объекте:
 * c_str() висячего временного std::string — это чтение освобождённой
 * памяти, и оно сработало бы не с первого раза. */
void feed(StreamBridge& bridge, const json& chunk) {
    const std::string dumped = chunk.dump();
    bridge.feed(dumped.c_str());
}

void feed(StreamBridge& bridge, const std::string& raw) {
    bridge.feed(raw.c_str());
}

json content_chunk(const std::string& text) {
    json c;
    c["choices"][0]["delta"]["content"] = text;
    c["choices"][0]["finish_reason"] = nullptr;
    return c;
}

json finish_chunk(const std::string& reason, int pt = 0, int ct = 0) {
    json c;
    c["choices"][0]["delta"] = json::object();
    c["choices"][0]["finish_reason"] = reason;
    if (pt || ct) {
        c["usage"]["prompt_tokens"] = pt;
        c["usage"]["completion_tokens"] = ct;
    }
    return c;
}

} // namespace

/* Дельты приходят по одной, в том порядке, в каком пришли. */
void test_text_deltas_arrive_one_by_one_in_order() {
    Recorder rec;
    StreamBridge bridge(&rec, Recorder::on_delta, Recorder::on_tool_delta);
    feed(bridge, content_chunk("При"));
    feed(bridge, content_chunk("вет"));
    feed(bridge, content_chunk("!"));

    TEST_ASSERT_EQUAL(rec.texts.size(), size_t(3));
    TEST_ASSERT_EQUAL(rec.text(), std::string("Привет!"));
    for (const auto& p : rec.texts) {
        TEST_ASSERT_EQUAL(p.kind, int(LLAMA_STREAM_DELTA_TEXT));
    }
}

/* Пустая дельта колбэк не вызывает: иначе UI получал бы событие на каждый
 * пустой чанк, а провайдеры их шлют пачками (первый чанк — пустой content). */
void test_empty_delta_is_not_delivered() {
    Recorder rec;
    StreamBridge bridge(&rec, Recorder::on_delta, Recorder::on_tool_delta);
    feed(bridge, content_chunk(""));
    feed(bridge, std::string("{\"choices\":[{\"delta\":{}}]}"));

    TEST_ASSERT_EQUAL(rec.texts.size(), size_t(0));
}

/* Размышление не должно попасть ни в колбэк текста, ни в итоговый ответ.
 *
 * Ключевая проверка второй половины: если бы reasoning копился в content_,
 * модель получила бы в истории свои рассуждения как слова ответа, и
 * следующий запрос стал бы длиннее без причины. */
void test_reasoning_is_not_part_of_the_answer() {
    Recorder rec;
    StreamBridge bridge(&rec, Recorder::on_delta, Recorder::on_tool_delta);

    json a;
    a["choices"][0]["delta"]["reasoning_content"] = "Думаю";
    feed(bridge, a);
    json b;
    b["choices"][0]["delta"]["reasoning"] = " ещё";
    feed(bridge, b);
    feed(bridge, content_chunk("Ответ"));
    feed(bridge, finish_chunk("stop"));

    TEST_ASSERT_EQUAL(rec.reasoning(), std::string("Думаю ещё"));
    TEST_ASSERT_EQUAL(rec.text(), std::string("Ответ"));
    const json done = json::parse(bridge.done_json(""));
    TEST_ASSERT_EQUAL(done["content"].get<std::string>(), std::string("Ответ"));
    TEST_ASSERT_EQUAL(done["ok"].get<int>(), 1);
}

/* Провайдер нумерует вызовы индексом, а не строкой. Без синтеза call_N две
 * параллельные сборки аргументов склеились бы в один вызов, и результат
 * второго инструмента достался бы не тому. */
void test_two_calls_by_index_do_not_merge() {
    Recorder rec;
    StreamBridge bridge(&rec, Recorder::on_delta, Recorder::on_tool_delta);

    json first;
    first["choices"][0]["delta"]["tool_calls"][0]["index"] = 0;
    first["choices"][0]["delta"]["tool_calls"][0]["function"]["name"] = "read";
    first["choices"][0]["delta"]["tool_calls"][0]["function"]["arguments"] = "{\"pa";
    feed(bridge, first);

    json second;
    second["choices"][0]["delta"]["tool_calls"][0]["index"] = 1;
    second["choices"][0]["delta"]["tool_calls"][0]["function"]["name"] = "grep";
    second["choices"][0]["delta"]["tool_calls"][0]["function"]["arguments"] = "{\"q";
    feed(bridge, second);

    json again_first;
    again_first["choices"][0]["delta"]["tool_calls"][0]["index"] = 0;
    again_first["choices"][0]["delta"]["tool_calls"][0]["function"]["arguments"] = "th\"}";
    feed(bridge, again_first);

    TEST_ASSERT_EQUAL(rec.tools.size(), size_t(3));
    TEST_ASSERT_EQUAL(rec.tools[0].call_id, std::string("call_0"));
    TEST_ASSERT_EQUAL(rec.tools[1].call_id, std::string("call_1"));
    TEST_ASSERT_EQUAL(rec.tools[2].call_id, std::string("call_0"));
    TEST_ASSERT_EQUAL(rec.tools[0].fragment + rec.tools[2].fragment,
                      std::string("{\"path\"}"));
}

/* Имя приходит только в первом фрагменте вызова, а куски аргументов идут
 * следующими. */
void test_tool_name_survives_later_fragments() {
    Recorder rec;
    StreamBridge bridge(&rec, Recorder::on_delta, Recorder::on_tool_delta);

    json first;
    first["choices"][0]["delta"]["tool_calls"][0]["index"] = 0;
    first["choices"][0]["delta"]["tool_calls"][0]["function"]["name"] = "write";
    first["choices"][0]["delta"]["tool_calls"][0]["function"]["arguments"] = "{";
    feed(bridge, first);

    json second;
    second["choices"][0]["delta"]["tool_calls"][0]["index"] = 0;
    second["choices"][0]["delta"]["tool_calls"][0]["function"]["arguments"] = "}";
    feed(bridge, second);

    TEST_ASSERT_EQUAL(rec.tools.size(), size_t(2));
    TEST_ASSERT_EQUAL(rec.tools[1].tool_name, std::string("write"));
}

/* Провайдер с идентификаторами отдаёт свой — он надёжнее синтетического,
 * потому что именно его возвращает последующий результат инструмента. */
void test_provider_call_id_wins_over_the_index() {
    Recorder rec;
    StreamBridge bridge(&rec, Recorder::on_delta, Recorder::on_tool_delta);

    json c;
    c["choices"][0]["delta"]["tool_calls"][0]["index"] = 3;
    c["choices"][0]["delta"]["tool_calls"][0]["id"] = "call_abc";
    c["choices"][0]["delta"]["tool_calls"][0]["function"]["name"] = "list";
    c["choices"][0]["delta"]["tool_calls"][0]["function"]["arguments"] = "{}";
    feed(bridge, c);

    TEST_ASSERT_EQUAL(rec.tools.size(), size_t(1));
    TEST_ASSERT_EQUAL(rec.tools[0].call_id, std::string("call_abc"));
}

/* Закрытый поток отдаёт тот же JSON, что и блокирующий llm_chat_messages:
 * ok/content/finish_reason/usage. */
void test_closed_stream_reports_the_whole_answer() {
    Recorder rec;
    StreamBridge bridge(&rec, Recorder::on_delta, Recorder::on_tool_delta);
    feed(bridge, content_chunk("Готово"));
    feed(bridge, finish_chunk("length", 120, 34));

    const json done = json::parse(bridge.done_json(""));
    TEST_ASSERT_EQUAL(done["ok"].get<int>(), 1);
    TEST_ASSERT_EQUAL(done["content"].get<std::string>(), std::string("Готово"));
    TEST_ASSERT_EQUAL(done["finish_reason"].get<std::string>(), std::string("length"));
    TEST_ASSERT_EQUAL(done["prompt_tokens"].get<int>(), 120);
    TEST_ASSERT_EQUAL(done["completion_tokens"].get<int>(), 34);
}

/* Оборванный поток — НЕ успешный пустой ответ.
 *
 * Это главная проверка файла: без неё обрыв сети выглядел бы как «модель
 * ничего не сказала», и ход агента закрылся бы с ответом пользователю. */
void test_truncated_stream_is_not_a_successful_empty_answer() {
    Recorder rec;
    StreamBridge bridge(&rec, Recorder::on_delta, Recorder::on_tool_delta);
    feed(bridge, content_chunk("Начало"));

    const json done = json::parse(bridge.done_json(""));
    TEST_ASSERT_EQUAL(done["ok"].get<int>(), 0);
    TEST_ASSERT(done["error"].get<std::string>().find("прерван") != std::string::npos);
    /* Накопленное не выбрасываем: плагин покажет его как частичный ответ. */
    TEST_ASSERT_EQUAL(done["content"].get<std::string>(), std::string("Начало"));
}

/* Ошибка провайдера важнее сообщения об обрыве: пользователю нужна причина
 * («HTTP 429»), а не «поток прерван». */
void test_provider_error_wins_over_the_truncation_message() {
    Recorder rec;
    StreamBridge bridge(&rec, Recorder::on_delta, Recorder::on_tool_delta);
    bridge.set_error("HTTP 429: лимит запросов провайдера");

    const json done = json::parse(bridge.done_json(""));
    TEST_ASSERT_EQUAL(done["ok"].get<int>(), 0);
    TEST_ASSERT_EQUAL(done["error"].get<std::string>(),
                      std::string("HTTP 429: лимит запросов провайдера"));
}

/* Мусор посреди потока не роняет ответ и не остаётся незамеченным: дырка в
 * середине текста иначе выглядит как связная речь. */
void test_malformed_chunk_is_counted_and_neighbours_survive() {
    Recorder rec;
    StreamBridge bridge(&rec, Recorder::on_delta, Recorder::on_tool_delta);
    feed(bridge, content_chunk("до"));
    feed(bridge, std::string("{это не json"));
    feed(bridge, std::string("42"));
    feed(bridge, content_chunk("после"));
    feed(bridge, finish_chunk("stop"));

    TEST_ASSERT_EQUAL(rec.text(), std::string("допосле"));
    const json done = json::parse(bridge.done_json(""));
    TEST_ASSERT_EQUAL(done["ok"].get<int>(), 1);
    TEST_ASSERT_EQUAL(done["skipped_chunks"].get<int>(), 2);
}

/* Без пропущенных чанков поля нет: плагин не должен искать ключ, которого
 * нет, и не должен читать «0 пропущено» как «пропущено неизвестно сколько». */
void test_no_skipped_chunks_field_when_all_chunks_were_good() {
    Recorder rec;
    StreamBridge bridge(&rec, Recorder::on_delta, Recorder::on_tool_delta);
    feed(bridge, content_chunk("чисто"));
    feed(bridge, finish_chunk("stop"));

    const json done = json::parse(bridge.done_json(""));
    TEST_ASSERT_FALSE(done.contains("skipped_chunks"));
}

/* Облачный путь отдаёт готовые куски, а не чанки. */
void test_push_text_accumulates_like_chunks() {
    Recorder rec;
    StreamBridge bridge(&rec, Recorder::on_delta, Recorder::on_tool_delta);
    bridge.push_text("а");
    bridge.push_text("б");
    bridge.set_usage(7, 3);
    bridge.set_finish_reason("stop");

    TEST_ASSERT_EQUAL(rec.text(), std::string("аб"));
    const json done = json::parse(bridge.done_json(""));
    TEST_ASSERT_EQUAL(done["ok"].get<int>(), 1);
    TEST_ASSERT_EQUAL(done["content"].get<std::string>(), std::string("аб"));
    TEST_ASSERT_EQUAL(done["prompt_tokens"].get<int>(), 7);
    TEST_ASSERT_EQUAL(done["completion_tokens"].get<int>(), 3);
}

/* Явная ошибка отменяет необходимость finish_reason. */
void test_error_marks_the_stream_closed() {
    Recorder rec;
    StreamBridge bridge(&rec, Recorder::on_delta, Recorder::on_tool_delta);
    TEST_ASSERT_FALSE(bridge.closed());
    bridge.set_error("отменено пользователем");
    TEST_ASSERT_TRUE(bridge.closed());
}

/* Пустой finish_reason (провайдер прислал null) закрытием не считается. */
void test_empty_finish_reason_does_not_close_the_stream() {
    Recorder rec;
    StreamBridge bridge(&rec, Recorder::on_delta, Recorder::on_tool_delta);
    bridge.set_finish_reason("");
    TEST_ASSERT_FALSE(bridge.closed());
    TEST_ASSERT_EQUAL(json::parse(bridge.done_json(""))["ok"].get<int>(), 0);
}

/* Синтетический идентификатор обязан совпадать с тем, что ждёт плагин:
 * префикс call_ и индекс. */
void test_call_id_for_index() {
    TEST_ASSERT_EQUAL(StreamBridge::call_id_for_index(0), std::string("call_0"));
    TEST_ASSERT_EQUAL(StreamBridge::call_id_for_index(12), std::string("call_12"));
}

/* Поля И6 добавлены в КОНЕЦ структуры хоста, в порядке объявления.
 *
 * Формулировка «последнее поле заканчивает структуру» выбрана не
 * случайно: хост пишет size = sizeof(LlamaHostApi), и плагин сверяется с
 * ним перед вызовом. Проверка «какое поле последнее» сломалась бы сама на
 * следующей итерации — а инвариант, который она защищала, при этом остался
 * бы в силе. Защищать надо «ничего не вставлено в середину», а не номер
 * последнего элемента: поле в середине сдвигает всё после себя, и старый
 * .so читает вместо строки мусор и падает без единого сообщения.
 *
 * Новое поле, добавленное в И6, вписывается сюда двумя строками: следующий
 * assert на смещение и перенос последнего в хвост структуры.
 */
void test_new_host_api_fields_are_append_only() {
    TEST_ASSERT_EQUAL(offsetof(LlamaHostApi, llm_chat_stream),
                      offsetof(LlamaHostApi, llm_chat_messages) + sizeof(void*));
    TEST_ASSERT_EQUAL(offsetof(LlamaHostApi, llm_ast_symbols),
                      offsetof(LlamaHostApi, llm_chat_stream) + sizeof(void*));
    TEST_ASSERT_EQUAL(offsetof(LlamaHostApi, llm_chat_cancel),
                      offsetof(LlamaHostApi, llm_ast_symbols) + sizeof(void*));
    /* Хвост структуры — последнее добавленное поле. Сюда же добавляется
     * assert для следующего поля; суть проверки — «ничего не вставлено
     * в середину». */
    TEST_ASSERT_EQUAL(offsetof(LlamaHostApi, llm_chat_cancel) + sizeof(void*),
                      sizeof(LlamaHostApi));
}

int main() {
    REGISTER_TEST(test_text_deltas_arrive_one_by_one_in_order);
    REGISTER_TEST(test_empty_delta_is_not_delivered);
    REGISTER_TEST(test_reasoning_is_not_part_of_the_answer);
    REGISTER_TEST(test_two_calls_by_index_do_not_merge);
    REGISTER_TEST(test_tool_name_survives_later_fragments);
    REGISTER_TEST(test_provider_call_id_wins_over_the_index);
    REGISTER_TEST(test_closed_stream_reports_the_whole_answer);
    REGISTER_TEST(test_truncated_stream_is_not_a_successful_empty_answer);
    REGISTER_TEST(test_provider_error_wins_over_the_truncation_message);
    REGISTER_TEST(test_malformed_chunk_is_counted_and_neighbours_survive);
    REGISTER_TEST(test_no_skipped_chunks_field_when_all_chunks_were_good);
    REGISTER_TEST(test_push_text_accumulates_like_chunks);
    REGISTER_TEST(test_error_marks_the_stream_closed);
    REGISTER_TEST(test_empty_finish_reason_does_not_close_the_stream);
    REGISTER_TEST(test_call_id_for_index);
    REGISTER_TEST(test_new_host_api_fields_are_append_only);

    return test::TestRunner::instance().run();
}
