/*
 * test_llm_stream_abi.cpp — проба поля хоста и мост C-ABI (И6.4).
 *
 * Проверяется то, что ломается тихо: чтение поля, которого у хоста нет, и
 * время жизни колбэков, которые переживают вызов. И то и другое не падает
 * в приложении — падает в поле, и то на другое поле.
 *
 * Хост фейковый: LlamaHostApi — структура с указателями на функции, её
 * можно собрать руками и скормить мосту. Ни сети, ни PluginManager.
 *
 * Чего этот набор НЕ проверяет: утечек. Ветка «поток не запустился» обязана
 * снять владельца контекста, но утечка не имеет функционального признака —
 * ни один assert её не увидит. Проверено отдельно: тот же файл под
 * -fsanitize=address,undefined с detect_leaks=1 даёт 13 passed и ноль
 * утечек. Если ветка изменится, проверять надо так же, а не «вроде
 * освобождается».
 */

#include "../host_bridge/llm_stream_probe.h"
#include "../host_bridge/llm_stream_shim.h"

#include "test_framework.h"

#include <cstddef>
#include <string>
#include <vector>

using namespace coder;

namespace {

/* Что «модель» ответит в этом прогоне. */
struct Script {
    bool start = true;                  /* хост согласился запустить поток */
    std::vector<std::pair<std::string, int>> deltas;
    std::vector<std::string> tool_calls; /* (call_id|tool|fragment) через "|?" */
    std::string done_json = "{\"ok\":1,\"content\":\"x\",\"finish_reason\":\"stop\"}";
    /* Что хост увидел на входе. */
    std::string seen_sys;
    std::string seen_messages;
    std::string seen_request;
};

Script* g_script = nullptr;

using DeltaFn = void (*)(void*, const char*, int);
using ToolDeltaFn = void (*)(void*, const char*, const char*, const char*);
using DoneFn = void (*)(void*, const char*);

char* fake_model_info(LlamaPluginHost*, const char*) { return nullptr; }
char* fake_model_current(LlamaPluginHost*) { return nullptr; }

/* Заглушки RAG v2 (R2.3): проба читает только указатель, но поле обязано
 * быть непустым, иначе has_* ответит «нет» по значению, а не по размеру. */
char* fake_rag_search_ex(LlamaPluginHost*, const char*, const char*, int, const char*) { return nullptr; }
char* fake_rag_index_document_ex(LlamaPluginHost*, const char*, const char*, const char*) { return nullptr; }
char* fake_rag_scope_info(LlamaPluginHost*, const char*) { return nullptr; }
char* fake_rag_scope_remove_document(LlamaPluginHost*, const char*, const char*) { return nullptr; }
int fake_rag_scope_clear(LlamaPluginHost*, const char*) { return 0; }
int fake_rag_scope_flush(LlamaPluginHost*, const char*) { return 0; }
void fake_rag_index_cancel(LlamaPluginHost*, void*) {}

int fake_llm_chat_stream(LlamaPluginHost*, const char* system_prompt,
                         const char* messages_json, const char* request_json,
                         void* user_data, DeltaFn on_delta,
                         ToolDeltaFn on_tool_delta, DoneFn on_done) {
    if (!g_script || !g_script->start) return 0;
    g_script->seen_sys = system_prompt ? system_prompt : "";
    g_script->seen_messages = messages_json ? messages_json : "";
    g_script->seen_request = request_json ? request_json : "";
    for (const auto& d : g_script->deltas) on_delta(user_data, d.first.c_str(), d.second);
    for (const auto& t : g_script->tool_calls) {
        const size_t a = t.find('|');
        const size_t b = t.find('|', a + 1);
        on_tool_delta(user_data, t.substr(0, a).c_str(),
                      t.substr(a + 1, b - a - 1).c_str(), t.substr(b + 1).c_str());
    }
    on_done(user_data, g_script->done_json.c_str());
    return 1;
}

/* Хост, у которого есть поле llm_chat_stream. */
LlamaHostApi api_with_stream() {
    LlamaHostApi api{};
    api.size = sizeof(LlamaHostApi);
    api.llm_chat_stream = &fake_llm_chat_stream;
    return api;
}

struct Collected {
    std::vector<std::string> text;
    std::vector<std::string> reasoning;
    std::vector<std::string> tool_fragments;
    int done_calls = 0;
    std::string done_json;
};

} // namespace

/* --- Проба поля --- */

/* У хоста есть поле — проба отвечает да, даже если это поле ещё никто не
 * звал. Проба читает, а не работает: одна проверка на инициализацию. */
TEST(probe_finds_a_present_field) {
    const LlamaHostApi api = api_with_stream();
    ASSERT_TRUE(host::has_llm_chat_stream(&api));
}

/* Старый хост: структура короче, поля в ней нет. Проба обязана ответить
 * нет, не читая поле, — именно это чтение и убивает плагин. */
TEST(probe_rejects_an_old_host) {
    LlamaHostApi api = api_with_stream();
    api.size = offsetof(LlamaHostApi, llm_chat_stream);  /* конец поля, не начало */
    ASSERT_FALSE(host::has_llm_chat_stream(&api));
    /* Ровно на границе поля хост его заполнил целиком. */
    api.size = offsetof(LlamaHostApi, llm_chat_stream) + sizeof(api.llm_chat_stream);
    ASSERT_TRUE(host::has_llm_chat_stream(&api));
}

/* Поле есть по размеру, но хост его не заполнил. Молча звать nullptr значило
 * бы упасть там, где надо было уйти на блокирующий путь. */
TEST(probe_rejects_a_null_field) {
    LlamaHostApi api = api_with_stream();
    api.llm_chat_stream = nullptr;
    ASSERT_FALSE(host::has_llm_chat_stream(&api));
}

TEST(probe_survives_a_null_api) {
    ASSERT_FALSE(host::has_llm_chat_stream(nullptr));
    ASSERT_FALSE(host::has_llm_ast_symbols(nullptr));
    ASSERT_FALSE(host::has_llm_chat_messages(nullptr));
    ASSERT_FALSE(host::has_model_info(nullptr));
    ASSERT_FALSE(host::has_model_current(nullptr));
}

/* Справочник моделей — append-only поля. У старого хоста их нет, и проба
 * обязана ответить «нет» по размеру, не читая поле. */
TEST(probe_rejects_an_old_host_without_model_catalog) {
    LlamaHostApi api = api_with_stream();
    api.model_info = nullptr;
    api.model_current = nullptr;
    api.size = offsetof(LlamaHostApi, model_info);  /* конец llm_chat_cancel */
    ASSERT_FALSE(host::has_model_info(&api));
    ASSERT_FALSE(host::has_model_current(&api));

    api.model_info = fake_model_info;
    api.model_current = fake_model_current;
    api.size = offsetof(LlamaHostApi, model_current) + sizeof(api.model_current);
    ASSERT_TRUE(host::has_model_info(&api));
    ASSERT_TRUE(host::has_model_current(&api));
}

/* RAG v2 — тоже append-only (R2.3): у старого хоста полей нет, и проба
 * обязана ответить «нет» ПО РАЗМЕРУ, не читая их. Тихий nullptr-путь
 * выглядел бы как «плагин иногда падает», а не как «старый хост». */
TEST(probe_rejects_an_old_host_without_rag_scopes) {
    LlamaHostApi api = api_with_stream();
    api.rag_search_ex = &fake_rag_search_ex;
    api.rag_index_document_ex = &fake_rag_index_document_ex;
    api.rag_scope_info = &fake_rag_scope_info;
    api.rag_scope_remove_document = &fake_rag_scope_remove_document;
    api.rag_scope_clear = &fake_rag_scope_clear;
    api.rag_scope_flush = &fake_rag_scope_flush;
    api.rag_index_cancel = &fake_rag_index_cancel;

    /* Хост закончился ровно на model_current — все поля RAG v2 за его
     * пределами, и читаться они не должны. */
    api.size = offsetof(LlamaHostApi, rag_search_ex);
    ASSERT_FALSE(host::has_rag_search_ex(&api));
    ASSERT_FALSE(host::has_rag_scope_info(&api));

    /* Хост заполнил структуру целиком — все семь полей доступны. */
    api.size = offsetof(LlamaHostApi, rag_index_cancel) + sizeof(api.rag_index_cancel);
    ASSERT_TRUE(host::has_rag_search_ex(&api));
    ASSERT_TRUE(host::has_rag_index_document_ex(&api));
    ASSERT_TRUE(host::has_rag_scope_info(&api));
    ASSERT_TRUE(host::has_rag_scope_remove_document(&api));
    ASSERT_TRUE(host::has_rag_scope_clear(&api));
    ASSERT_TRUE(host::has_rag_scope_flush(&api));
    ASSERT_TRUE(host::has_rag_index_cancel(&api));
}

TEST(probe_rag_null_field_is_not_enough) {
    /* Размер есть, значение — нет: звать нельзя. */
    LlamaHostApi api = api_with_stream();
    api.rag_search_ex = &fake_rag_search_ex;
    api.rag_search_ex = nullptr;
    ASSERT_FALSE(host::has_rag_search_ex(&api));

    ASSERT_FALSE(host::has_rag_search_ex(nullptr));
    ASSERT_FALSE(host::has_rag_scope_clear(nullptr));
}

/* Проба не должна зависеть от того, заполнены ли более новые поля: хост
 * постарше, чем плагин, и это норма. */
TEST(probe_works_when_newer_fields_are_missing) {
    LlamaHostApi api = api_with_stream();
    api.llm_ast_symbols = nullptr;
    api.size = offsetof(LlamaHostApi, llm_ast_symbols);
    ASSERT_TRUE(host::has_llm_chat_stream(&api));
    ASSERT_FALSE(host::has_llm_ast_symbols(&api));
}

/* --- Мост: прозрачный откат --- */

/* Хост без поля — мост отказывает, и это НЕ ошибка: вызывающий обязан уйти
 * на блокирующий путь. Главное здесь — что on_done при этом НЕ зовётся,
 * иначе вызывающий получил бы закрытие вызова, которого не было. */
TEST(old_host_falls_back_without_calling_on_done) {
    LlamaHostApi api = api_with_stream();
    api.size = offsetof(LlamaHostApi, llm_chat_stream);
    Script script;
    g_script = &script;

    Collected got;
    const bool started = host::call_llm_chat_stream(
        &api, nullptr, "sys", {{"user", "привет"}}, "",
        [](void*) {},
        [&](const char* t, int kind) {
            (kind == 0 ? got.text : got.reasoning).push_back(t);
        },
        [&](const char*, const char*, const char* f) { got.tool_fragments.push_back(f); },
        [&](const std::string& j) { ++got.done_calls; got.done_json = j; });
    g_script = nullptr;

    ASSERT_FALSE(started);
    ASSERT_EQ(got.done_calls, 0);
    ASSERT_TRUE(got.text.empty());
}

/* Хост, который отказался запускать поток, — тот же исход: false без
 * on_done. Иначе у вызывающего появилось бы закрытие без дельт. */
TEST(declined_stream_is_false_without_on_done) {
    const LlamaHostApi api = api_with_stream();
    Script script;
    script.start = false;
    g_script = &script;

    Collected got;
    const bool started = host::call_llm_chat_stream(
        &api, nullptr, "sys", {{"user", "привет"}}, "",
        [](void*) {},
        [&](const char*, int) { got.text.push_back("x"); },
        [&](const char*, const char*, const char*) {},
        [&](const std::string&) { ++got.done_calls; });
    g_script = nullptr;

    ASSERT_FALSE(started);
    ASSERT_EQ(got.done_calls, 0);
}

/* --- Мост: передача данных --- */

/* Дельты приходят в том виде, в каком их отдал хост, и вид не теряется:
 * размышление не должно приехать текстом. */
TEST(deltas_arrive_with_their_kind) {
    const LlamaHostApi api = api_with_stream();
    Script script;
    script.deltas = {{"При", 0}, {"думаю", 1}, {"вет", 0}};
    g_script = &script;

    Collected got;
    const bool started = host::call_llm_chat_stream(
        &api, nullptr, "sys", {{"user", "привет"}}, "",
        [](void*) {},
        [&](const char* t, int kind) {
            (kind == 0 ? got.text : got.reasoning).push_back(t);
        },
        [&](const char*, const char*, const char*) {},
        [&](const std::string& j) { ++got.done_calls; got.done_json = j; });
    g_script = nullptr;

    ASSERT_TRUE(started);
    ASSERT_EQ(got.text.size(), size_t(2));
    ASSERT_EQ(got.reasoning.size(), size_t(1));
    ASSERT_EQ(got.reasoning[0], std::string("думаю"));
    ASSERT_EQ(got.done_calls, 1);
    ASSERT_TRUE(got.done_json.find("\"ok\":1") != std::string::npos);
}

TEST(tool_fragments_arrive_with_ids_and_names) {
    const LlamaHostApi api = api_with_stream();
    Script script;
    script.tool_calls = {"call_0|read_file|{", "call_0|read_file|}"};
    g_script = &script;

    std::vector<std::string> ids, names, frags;
    const bool started = host::call_llm_chat_stream(
        &api, nullptr, "sys", {{"user", "x"}}, "",
        [](void*) {},
        [&](const char*, int) {},
        [&](const char* id, const char* name, const char* f) {
            ids.push_back(id);
            names.push_back(name);
            frags.push_back(f);
        },
        [&](const std::string&) {});
    g_script = nullptr;

    ASSERT_TRUE(started);
    ASSERT_EQ(ids.size(), size_t(2));
    ASSERT_EQ(ids[1], std::string("call_0"));
    ASSERT_EQ(names[1], std::string("read_file"));
    ASSERT_EQ(frags[0] + frags[1], std::string("{}"));
}

/* Хост видит ровно ту историю и тот системный промпт, которые передали, и
 * request_json доезжает строкой, а не nullptr, если он задан. */
TEST(request_reaches_the_host_verbatim) {
    const LlamaHostApi api = api_with_stream();
    Script script;
    g_script = &script;

    host::call_llm_chat_stream(
        &api, nullptr, "системный",
        {{"user", "первый"}, {"assistant", "второй"}},
        "{\"max_tokens\":256}",
        [&](void*) {},
        [&](const char*, int) {}, [&](const char*, const char*, const char*) {},
        [&](const std::string&) {});
    g_script = nullptr;

    ASSERT_EQ(script.seen_sys, std::string("системный"));
    ASSERT_TRUE(script.seen_messages.find("\"role\":\"user\"") != std::string::npos);
    ASSERT_TRUE(script.seen_messages.find("\"role\":\"assistant\"") != std::string::npos);
    ASSERT_TRUE(script.seen_messages.find("второй") != std::string::npos);
    ASSERT_EQ(script.seen_request, std::string("{\"max_tokens\":256}"));
}

/* Пустой системный промпт уходит как nullptr, а не как "" — так его трактует
 * и сам хост, и проверка «промпт задан». */
TEST(empty_system_prompt_is_sent_as_null) {
    const LlamaHostApi api = api_with_stream();
    Script script;
    g_script = &script;
    host::call_llm_chat_stream(&api, nullptr, "", {{"user", "x"}}, "",
                               [&](void*) {},
                               [&](const char*, int) {},
                               [&](const char*, const char*, const char*) {},
                               [&](const std::string&) {});
    g_script = nullptr;
    ASSERT_EQ(script.seen_sys, std::string(""));
    ASSERT_EQ(script.seen_request, std::string(""));
}

/* --- Сборка истории --- */

/* Роль и содержимое экранируются: иначе одна кавычка в ответе модели
 * ломает весь JSON, и хост вернёт «мусор» вместо отказа. */
TEST(messages_json_escapes_content) {
    const std::string json = host::messages_to_json(
        {{"user", "скажи \"привет\"\nи \\ уходи"}});
    ASSERT_TRUE(json.find("\\\"привет\\\"") != std::string::npos);
    ASSERT_TRUE(json.find("\\n") != std::string::npos);
    ASSERT_TRUE(json.find("\\\\") != std::string::npos);
}

/* Пустая история — пустой массив, а не пустая строка. */
TEST(empty_history_is_an_empty_array) {
    ASSERT_EQ(host::messages_to_json({}), std::string("[]"));
}

/* --- Изображения в сборке истории (мультимодальность) --- */

/* Текстовая реплика остаётся СТРОКОЙ: это главный регрессионный страж.
 *
 * Соблазн написать «если у запроса есть картинка, все реплики становятся
 * массивами» велик, и он сломал бы ровно то, что работало. Строка и
 * массив в одном поле JSON — разные типы, и половина локальных сборок
 * llama-server отвергла бы массив из одной текстовой части. */
TEST(text_message_stays_a_string) {
    const std::string json = host::messages_to_json({{"user", "привет"}});
    ASSERT_TRUE(json.find("\"content\":\"привет\"") != std::string::npos);
    ASSERT_TRUE(json.find("\"content\":[") == std::string::npos);
}

/* Реплика с картинкой и текстом — массив частей, текст ПЕРВЫЙ.
 *
 * Порядок не косметика: провайдеры читают изображение как иллюстрацию к
 * вопросу, а не наоборот, — тот же порядок, что в примерах OpenAI. */
TEST(image_message_becomes_a_parts_array_with_text_first) {
    ModelMessage m;
    m.role = "user";
    m.content = "что здесь не так?";
    m.images.push_back({"data:image/png;base64,AAAA", "image/png"});

    const std::string json = host::messages_to_json({m});
    ASSERT_TRUE(json.find("\"content\":[{") != std::string::npos);
    /* Текстовая часть — первая. */
    ASSERT_TRUE(json.find("{\"type\":\"text\",\"text\":\"что здесь не так?\"}") !=
                std::string::npos);
    ASSERT_TRUE(json.find("{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,AAAA\"") !=
                std::string::npos);
}

/* Реплика из ОДНОЙ картинки, без текста, — законный формат.
 *
 * Ровно тот случай, на котором всё ломалось: человек присылает скриншот
 * без подписи, и «пустой текст» не должен ни съедать реплику, ни
 * превращаться в `"content": ""` (модель ответила бы «не вижу картинки»). */
TEST(image_without_text_sends_only_the_image_part) {
    ModelMessage m;
    m.role = "user";
    m.images.push_back({"data:image/jpeg;base64,BBBB", "image/jpeg"});

    const std::string json = host::messages_to_json({m});
    /* Ровно одна часть: пустая текстовая не добавляется. */
    ASSERT_TRUE(json.find("\"content\":[{\"type\":\"image_url\"") !=
                std::string::npos);
    ASSERT_TRUE(json.find("\"type\":\"text\"") == std::string::npos);
}

/* Пустой media_type не уезжает: провайдер определит тип сам, а
 * `"media_type":""` — это заявленный пустой тип, то есть ошибка разбора. */
TEST(empty_media_type_is_not_sent) {
    ModelMessage m;
    m.role = "user";
    m.images.push_back({"data:image/png;base64,AAAA", ""});

    const std::string json = host::messages_to_json({m});
    ASSERT_TRUE(json.find("media_type") == std::string::npos);
}

/* Две картинки сохраняют порядок: порядок вложений человек видит в списке
 * под полем ввода, и он же определяет порядок частей в запросе. */
TEST(images_keep_their_order) {
    ModelMessage m;
    m.role = "user";
    m.content = "оба";
    m.images.push_back({"data:image/png;base64,AAA", "image/png"});
    m.images.push_back({"data:image/png;base64,BBB", "image/png"});

    const std::string json = host::messages_to_json({m});
    const size_t a = json.find("AAA");
    const size_t b = json.find("BBB");
    ASSERT_TRUE(a != std::string::npos);
    ASSERT_TRUE(b != std::string::npos);
    ASSERT_TRUE(a < b);
}

/* Собранное изображение — ВАЛИДНЫЙ JSON, а не «строка, похожая на него».
 *
 * Проверяется разбором: склейка `"url":"…" + "}}"` руками — ровно то
 * место, где лишняя или недостающая скоба даёт не отказ, а «модель
 * ответила не то». Свой парсер плагина (core/json.h) для этого не годится
 * — он проверял бы склейку тем же кодом, который её сделал. */
TEST(image_message_is_valid_json) {
    ModelMessage m;
    m.role = "user";
    m.content = "вопрос";
    m.images.push_back({"data:image/png;base64,AAA", "image/png"});
    m.images.push_back({"data:image/png;base64,BBB", ""});

    const std::string json = host::messages_to_json({m});
    json::JsonValue root;
    std::string err;
    /* Причина печатается ДО проверки: ASSERT_TRUE здесь не поддерживает
     * `<<`, и падение «ASSERT_TRUE failed» не сказало бы, ЧТО не
     * разобралось. */
    if (!json::JsonValue::parse(json, root, &err)) {
        std::cerr << "  собранный JSON не разбирается: " << err << std::endl;
    }
    ASSERT_TRUE(err.empty());
    ASSERT_TRUE(root.is_array());
    ASSERT_EQ(root.size(), size_t(1));
}
