#pragma once

/*
 * llm_stream_probe.h — проверка, есть ли у хоста нужное поле (И6.4).
 *
 * Живёт в host_bridge/, потому что видит LlamaHostApi. Ядро плагина (core/)
 * от хоста не зависит (D-7), и единственный код, которому ABI положено
 * знать, — точка входа и этот слой. Именно «слой», а не каталог src/:
 * src/ — только точка входа (правило из И0.9, проверяется тестом), а мост
 * не точка входа, а слой над границей.
 *
 * Зачем вообще проверка: структура LlamaHostApi растёт только в конец, и у
 * плагина, собранного против старого заголовка, поля может не быть. Чтение
 * такого поля — это выход за пределы того, что хост заполнил.
 */

#include "plugins/plugin_api.h"

#include <cstddef>

namespace coder {
namespace host {

/*
 * Общая форма проверки, записанная один раз, чтобы её нельзя было выполнить
 * неправильно в новом поле.
 *
 * ВНИМАНИЕ, и это главное: сюда НЕЛЬЗЯ передавать само поле
 * (host_field_present(api->size, api->llm_chat_stream)). Аргументы
 * вычисляются ДО вызова, то есть поле читается раньше, чем проверена длина
 * структуры, — а это ровно то чтение, ради которого проверка и нужна.
 * Поэтому ниже порядок записан руками в каждой функции, и sizeof(field)
 * там не оценивается (sizeof значения не читает).
 */

/* Есть ли у хоста llm_chat_messages (И1). */
bool has_llm_chat_messages(const LlamaHostApi* api);

/* Есть ли у хоста llm_chat_stream (И6.4). */
bool has_llm_chat_stream(const LlamaHostApi* api);

/* Есть ли у хоста llm_ast_symbols (И6.3). */
bool has_llm_ast_symbols(const LlamaHostApi* api);

/* Есть ли у хоста llm_chat_cancel (И6.6) — то есть умеет ли хост
 * останавливать генерацию, а не только чтение. */
bool has_llm_chat_cancel(const LlamaHostApi* api);

/* Есть ли у хоста справочник моделей (шаг 2 хоста): model_info/model_current.
 * Старый плагин их не читает, новый обязан проверить offsetof: поля в конце
 * структуры, и у старого хоста за его `size` лежит чужое. */
bool has_model_info(const LlamaHostApi* api);
bool has_model_current(const LlamaHostApi* api);

/* --- RAG v2: скоупы (R2.3) ---
 *
 * Легаси-функции rag_* есть у всех, а эти поля добавлены в конец ABI
 * (R2.1) и у старого хоста их нет. Плагин обязан проверить ПЕРЕД вызовом:
 * чтение поля за пределами `size` — это чтение чужой памяти, и оно
 * выглядит как «плагин иногда падает», а не как «старый хост».
 *
 * Если has_rag_search_ex == false, плагин уходит на легаси-путь
 * (rag_search в скоупе app) — так R5.2 и написано. */
bool has_rag_search_ex(const LlamaHostApi* api);
bool has_rag_index_document_ex(const LlamaHostApi* api);
bool has_rag_scope_info(const LlamaHostApi* api);
bool has_rag_scope_remove_document(const LlamaHostApi* api);
bool has_rag_scope_clear(const LlamaHostApi* api);
bool has_rag_scope_flush(const LlamaHostApi* api);
bool has_rag_index_cancel(const LlamaHostApi* api);

} // namespace host
} // namespace coder
