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

bool has_model_info(const LlamaHostApi* api)
{
    if (!api) return false;
    if (api->size < offsetof(LlamaHostApi, model_info) +
                        sizeof(api->model_info)) {
        return false;
    }
    return api->model_info != nullptr;
}

bool has_model_current(const LlamaHostApi* api)
{
    if (!api) return false;
    if (api->size < offsetof(LlamaHostApi, model_current) +
                        sizeof(api->model_current)) {
        return false;
    }
    return api->model_current != nullptr;
}

/* --- RAG v2 (R2.3). Порядок тот же: сначала размер, потом значение —
 * иначе чтение поля за пределами `size` (см. шапку заголовка). */

bool has_rag_search_ex(const LlamaHostApi* api)
{
    if (!api) return false;
    if (api->size < offsetof(LlamaHostApi, rag_search_ex) +
                        sizeof(api->rag_search_ex)) {
        return false;
    }
    return api->rag_search_ex != nullptr;
}

bool has_rag_index_document_ex(const LlamaHostApi* api)
{
    if (!api) return false;
    if (api->size < offsetof(LlamaHostApi, rag_index_document_ex) +
                        sizeof(api->rag_index_document_ex)) {
        return false;
    }
    return api->rag_index_document_ex != nullptr;
}

bool has_rag_scope_info(const LlamaHostApi* api)
{
    if (!api) return false;
    if (api->size < offsetof(LlamaHostApi, rag_scope_info) +
                        sizeof(api->rag_scope_info)) {
        return false;
    }
    return api->rag_scope_info != nullptr;
}

bool has_rag_scope_remove_document(const LlamaHostApi* api)
{
    if (!api) return false;
    if (api->size < offsetof(LlamaHostApi, rag_scope_remove_document) +
                        sizeof(api->rag_scope_remove_document)) {
        return false;
    }
    return api->rag_scope_remove_document != nullptr;
}

bool has_rag_scope_clear(const LlamaHostApi* api)
{
    if (!api) return false;
    if (api->size < offsetof(LlamaHostApi, rag_scope_clear) +
                        sizeof(api->rag_scope_clear)) {
        return false;
    }
    return api->rag_scope_clear != nullptr;
}

bool has_rag_scope_flush(const LlamaHostApi* api)
{
    if (!api) return false;
    if (api->size < offsetof(LlamaHostApi, rag_scope_flush) +
                        sizeof(api->rag_scope_flush)) {
        return false;
    }
    return api->rag_scope_flush != nullptr;
}

bool has_rag_index_cancel(const LlamaHostApi* api)
{
    if (!api) return false;
    if (api->size < offsetof(LlamaHostApi, rag_index_cancel) +
                        sizeof(api->rag_index_cancel)) {
        return false;
    }
    return api->rag_index_cancel != nullptr;
}

} // namespace host
} // namespace coder
