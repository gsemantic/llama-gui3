// llm_stream_shim.cpp — мост от C-ABI хоста к std::function плагина (И6.4).

#include "llm_stream_shim.h"

#include "core/json_utils.h"

#include <memory>

namespace coder {
namespace host {

std::string messages_to_json(const std::vector<ModelMessage>& messages)
{
    std::string json = "[";
    for (size_t i = 0; i < messages.size(); ++i) {
        if (i) json += ",";
        const ModelMessage& m = messages[i];
        json += "{\"role\":\"" + json::escape(m.role) + "\",\"content\":";
        if (m.images.empty()) {
            /* Текстовая реплика — СТРОКОЙ, как было до мультимодальности.
             * Ветка стоит на непустом списке, а не на «умеет ли модель»:
             * иначе каждый обычный запрос уезжал бы массивом из одной
             * части, и половина локальных сборок llama-server отвергла бы
             * его (строка и массив — разные типы). */
            json += "\"" + json::escape(m.content) + "\"";
        } else {
            /* Массив частей OpenAI-формата. Текстовая часть первой:
             * провайдеры читают изображение как иллюстрацию к вопросу, а не
             * наоборот, — тот же порядок, что в примерах OpenAI.
             *
             * Пустой текст не добавляется: часть с пустым text половина
             * шлюзов считает ошибкой разбора (то же правило, что у пустых
             * кусков в core/message.h). */
            json += "[";
            bool first_part = true;
            if (!m.content.empty()) {
                json += "{\"type\":\"text\",\"text\":\"" +
                        json::escape(m.content) + "\"}";
                first_part = false;
            }
            for (const ImagePart& img : m.images) {
                if (img.url.empty()) continue;
                if (!first_part) json += ",";
                first_part = false;
                json += "{\"type\":\"image_url\",\"image_url\":{\"url\":\"" +
                        json::escape(img.url) + "\"";
                if (!img.media_type.empty()) {
                    json += ",\"media_type\":\"" +
                            json::escape(img.media_type) + "\"";
                }
                json += "}}";
            }
            /* Массив не может кончиться пустым: реплика без текста и без
             * картинок сюда не попадает (flush в to_model_messages), а если
             * бы попала — лучше пустая строка, чем «content: []», которую
             * провайдер считает ошибкой. */
            json += first_part ? "\"\"" : "]";
        }
        json += "}";
    }
    json += "]";
    return json;
}

bool cancel_llm_chat_stream(const LlamaHostApi* api, void* handle)
{
    if (!has_llm_chat_cancel(api)) return false;
    api->llm_chat_cancel(nullptr, handle);
    return true;
}

struct StreamCallContext {
    std::function<void(const char*, int)> on_delta;
    std::function<void(const char*, const char*, const char*)> on_tool_delta;
    std::function<void(const std::string&)> on_done;
};

/* Трамплины хоста. Объявления C-совместимы: сигнатура колбэка в ABI задана
 * хостом, и подмена её std::function-типов ломает вызов на границе.
 *
 * user_data у ВСЕХ трёх — владелец (указатель на shared_ptr), а не сам
 * контекст. Это обязано быть одинаково везде: если on_done разворачивает
 * владение, а on_delta читает указатель как сам контекст, то адрес
 * интерпретируется двумя разными способами, и согласование падает в
 * полях. */
namespace {

StreamCallContext* context_of(void* user_data)
{
    auto* owner = static_cast<std::shared_ptr<StreamCallContext>*>(user_data);
    return owner ? owner->get() : nullptr;
}

} // namespace

extern "C" {
static void shim_on_delta(void* user_data, const char* text, int kind) {
    StreamCallContext* ctx = context_of(user_data);
    if (ctx && ctx->on_delta) ctx->on_delta(text, kind);
}

static void shim_on_tool_delta(void* user_data, const char* call_id,
                               const char* tool_name, const char* fragment) {
    StreamCallContext* ctx = context_of(user_data);
    if (ctx && ctx->on_tool_delta) ctx->on_tool_delta(call_id, tool_name, fragment);
}

static void shim_on_done(void* user_data, const char* result_json) {
    /* Забираем владение и стираем общий указатель, чтобы второй on_done
     * (нарушение контракта хоста) не оказался вызовом уже освобождённой
     * памяти. */
    auto* owner = static_cast<std::shared_ptr<StreamCallContext>*>(user_data);
    if (!owner) return;
    std::shared_ptr<StreamCallContext> ctx = std::move(*owner);
    delete owner;
    if (!ctx) return;
    std::function<void(const std::string&)> done;
    done.swap(ctx->on_done);
    ctx->on_delta = nullptr;
    ctx->on_tool_delta = nullptr;
    if (done) done(result_json ? std::string(result_json) : std::string("{\"ok\":0}"));
}
} // extern "C"

bool call_llm_chat_stream(const LlamaHostApi* api, LlamaPluginHost* host,
                          const std::string& sys_prompt,
                          const std::vector<ModelMessage>& messages,
                          const std::string& request_json,
                          const std::function<void(void*)>& on_started,
                          const std::function<void(const char*, int)>& on_delta,
                          const std::function<void(const char*, const char*, const char*)>& on_tool_delta,
                          const std::function<void(const std::string&)>& on_done)
{
    if (!has_llm_chat_stream(api)) return false;
    if (!api->llm_chat_stream) return false;

    auto context = std::make_shared<StreamCallContext>();
    context->on_delta = on_delta;
    context->on_tool_delta = on_tool_delta;
    context->on_done = on_done;

    /* Общий указатель в куче, а не локальная переменная: колбэки приходят
     * из потока хоста уже после возврата, и локальная копия разделяемого
     * указателя умерла бы вместе с вызовом, оставив хост с сырым
     * указателем на освобождённый объект. Владельца забирает on_done. */
    auto* owner = new std::shared_ptr<StreamCallContext>(context);

    /* handle сообщается ДО входа в хоста: пока хост не ответил «поток
     * запущен», отменять нечего, а вызывающий не должен узнавать handle
     * постфактум. */
    if (on_started) on_started(context.get());

    const int rc = api->llm_chat_stream(
        host, sys_prompt.empty() ? nullptr : sys_prompt.c_str(),
        messages_to_json(messages).c_str(),
        request_json.empty() ? nullptr : request_json.c_str(),
        owner, &shim_on_delta, &shim_on_tool_delta, &shim_on_done);

    if (rc != 1) {
        /* Поток не запущен: on_done не будет. Владельца снимаем здесь, иначе
         * на каждый отклонённый вызов жил бы контекст до конца сессии. */
        delete owner;
        return false;
    }
    return true;
}

} // namespace host
} // namespace coder
