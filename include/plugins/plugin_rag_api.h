#pragma once

/*
 * plugin_rag_api.h — реализация ABI RAG v2 (R2.2).
 *
 * ЗАЧЕМ ОТДЕЛЬНЫЙ ФАЙЛ. `host_rag_*` в plugin_manager.cpp лежат в
 * анонимном namespace, и тест их не видит (та же причина, по которой
 * гейт R0.2 живёт в plugin_rag_gate.cpp, а разбор символов — в
 * ast_symbols.cpp). Здесь только чистая логика: «что ответить плагину»,
 * без `LlamaPluginHost*` и без обращения к глобальному состоянию.
 *
 * ЧТО ЗДЕСЬ ЕСТЬ.
 *   - resolve_scope: имя скоупа, которое просит плагин, сверяется с
 *     идентичностью плагина. Плагин НЕ может назвать чужой скоуп:
 *     `plugin:<name>` или `plugin:<name>:<suffix>`, где name — его
 *     собственное имя от хоста.
 *   - *_impl: сами вызовы. Возвращают malloc-строку (освобождает
 *     вызывающий, в ABI — `free_string`), как и остальной ABI.
 *   - plugin_has_permission: проверка права из манифеста (R3.1).
 *
 * КОДЫ ОТКАЗА — единственный источник строк, которые видит плагин:
 *   disabled      — RAG выключен пользователем или хостом (R0.2);
 *   unavailable   — реестр скоупов не настроен (нет RAG-инфраструктуры);
 *   denied        — у плагина нет нужного права (R3.1; `_ex` отказывают);
 *   bad_scope     — скоуп не принадлежит плагину или имя невалидно;
 *   bad_request   — пустой запрос/путь, k <= 0;
 *   quota         — R4.2 (квоты), здесь не используется;
 *   bad_index     — индекс повреждён/несовместим, R4.
 */

#include "core/rag_registry.h"

#include <string>
#include <vector>

namespace llama_gui {
namespace plugin {

/* Права RAG из манифеста (plugin.json). Строки — те же, что в схеме
 * манифеста; здесь только сравнение, не разбор.
 *
 *   rag_access — чтение: поиск, эмбеддинги, info, flush, отмена;
 *   rag_write  — изменение: индексация, удаление документа, очистка.
 *
 * Ротация (см. план, §3.4): в R3 `_ex`-функции ОТКАЗЫВАЮТ без права,
 * легаси-функции пока только ПРЕДУПРЕЖДАЮТ. До R3 ни одно право не
 * применялось — манифест был информационным. */
bool plugin_has_permission(const std::vector<std::string>& permissions,
                           const std::string& required);

/* Контекст одного вызова. Собирает вызывающий (host_rag_*), сами функции
 * контекст не добывают: так их можно тестировать без PluginManager и без
 * глобального состояния. */
struct RagCallContext {
    core::RagRegistry* registry = nullptr;
    bool allowed = false;          /* R0.2: settings.rag().enable_rag и rag_manager */
    std::string plugin_name;       /* PluginInfo::name хоста (уже санитизирован) */
    /* R3.1: права плагина из манифеста. Заполняет хост; `_ex`-функции
     * отказывают без нужного права. По умолчанию false — «нет манифеста,
     * нет права»: плагин без plugin.json не получает доступ к данным
     * пользователя по умолчанию. */
    bool has_rag_access = false;
    bool has_rag_write = false;
};

/* Разобрать имя скоупа, которое просит плагин.
 *
 * Возвращает СУФФИКС (пустой = скоуп плагина по умолчанию), а не полное
 * имя: полное имя строит реестр — так имя нельзя «подделать» строкой,
 * прошедшей проверку в одном месте и собранной в другом.
 *
 * Валидно: "", "plugin:<name>", "plugin:<name>:<suffix>".
 * Отказ: любое другое (чужой плагин, `..`, `/`, пустой суффикс после `:`
 * и т.п.). `error` заполняется причиной — она уйдёт плагину.
 */
bool resolve_scope(const std::string& plugin_name,
                   const std::string& requested,
                   std::string& suffix_out,
                   std::string& error);

/* --- Реализации вызовов ABI (см. plugin_api.h, RAG v2) --- */

char* rag_search_ex_impl(const RagCallContext& ctx,
                         const std::string& requested_scope,
                         const std::string& query, int k,
                         const std::string& path_filter);

char* rag_index_document_ex_impl(const RagCallContext& ctx,
                                 const std::string& requested_scope,
                                 const std::string& path,
                                 const std::string& opts_json);

char* rag_scope_info_impl(const RagCallContext& ctx,
                          const std::string& requested_scope);

char* rag_scope_remove_document_impl(const RagCallContext& ctx,
                                     const std::string& requested_scope,
                                     const std::string& path);

int   rag_scope_clear_impl(const RagCallContext& ctx,
                           const std::string& requested_scope);

int   rag_scope_flush_impl(const RagCallContext& ctx,
                           const std::string& requested_scope);

} // namespace plugin
} // namespace llama_gui