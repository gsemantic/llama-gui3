// llm_stream_probe.cpp — проверка наличия полей хоста (И6.4).

#include "llm_stream_probe.h"

namespace coder {
namespace host {

/*
 * size — сколько байт заполнил ХОСТ своей версией структуры. У хоста старее
 * нас sizeof меньше, и наше поле оказывается за его пределами.
 * Сравнение строгое «меньше»: при size ровно на конце поля хост заполнил
 * его целиком, и поле читать можно.
 */

bool has_llm_chat_messages(const LlamaHostApi* api)
{
    if (!api) return false;
    if (api->size < offsetof(LlamaHostApi, llm_chat_messages) +
                        sizeof(api->llm_chat_messages)) {
        return false;
    }
    return api->llm_chat_messages != nullptr;
}

bool has_llm_chat_stream(const LlamaHostApi* api)
{
    if (!api) return false;
    if (api->size < offsetof(LlamaHostApi, llm_chat_stream) +
                        sizeof(api->llm_chat_stream)) {
        return false;
    }
    return api->llm_chat_stream != nullptr;
}

bool has_llm_ast_symbols(const LlamaHostApi* api)
{
    if (!api) return false;
    if (api->size < offsetof(LlamaHostApi, llm_ast_symbols) +
                        sizeof(api->llm_ast_symbols)) {
        return false;
    }
    return api->llm_ast_symbols != nullptr;
}

bool has_llm_chat_cancel(const LlamaHostApi* api)
{
    if (!api) return false;
    if (api->size < offsetof(LlamaHostApi, llm_chat_cancel) +
                        sizeof(api->llm_chat_cancel)) {
        return false;
    }
    return api->llm_chat_cancel != nullptr;
}

} // namespace host
} // namespace coder
