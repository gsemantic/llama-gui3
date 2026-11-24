/*
 * plugin_rag_api.cpp — реализация ABI RAG v2 (R2.2).
 *
 * Обоснование и границы — в plugin_rag_api.h. Здесь только код.
 *
 * ПОЧЕМУ КАЖДАЯ ФУНКЦИЯ НАЧИНАЕТСЯ ОДИНАКОВО. Порядок «выключено →
 * реестра нет → скоуп не твой» — это не украшение: плагин, который
 * получает `bad_scope` на выключенном RAG, будет искать ошибку в своём
 * имени, а не в настройках пользователя. Общий вход `begin()` записан
 * один раз, чтобы порядок нельзя было перепутать в шести местах.
 */

#include "plugins/plugin_rag_api.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace llama_gui {
namespace plugin {

namespace {

using json = nlohmann::json;

/* Копия строки в malloc: ABI требует, чтобы плагин освобождал ответ
 * `free_string`, и тот вызывает free(). strdup() на MSVC — «deprecated»,
 * а поведение нам нужно ровно одно. */
char* dup_string(const std::string& s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

char* error_json(const std::string& code, const std::string& message) {
    json j;
    j["ok"] = false;
    j["code"] = code;
    j["error"] = message;
    return dup_string(j.dump());
}

/* Какое право нужно вызову. Значение, а не строка: сравнение «нужное
 * право == выданное» не должно зависеть от того, как назвали строку в
 * манифесте и в коде. */
enum class RequiredPermission { Access, Write };

const char* permission_name(RequiredPermission p) {
    return p == RequiredPermission::Write ? "rag_write" : "rag_access";
}

bool permission_granted(const RagCallContext& ctx, RequiredPermission p) {
    return p == RequiredPermission::Write ? ctx.has_rag_write
                                          : ctx.has_rag_access;
}

/* Общий вход: контекст + имя скоупа от плагина.
 *
 * Возвращает true и заполняет `suffix`, либо false и готовый JSON отказа
 * в `*out_error` (его и возвращает вызывающая функция как есть).
 *
 * Порядок проверок — часть контракта, см. шапку файла: выключено →
 * реестра нет → нет права → скоуп не твой. Право проверяется ДО скоупа:
 * плагин без `rag_access` не должен получать `bad_scope` (тот увёл бы
 * поиск в имя, а не в манифест), ровно как выключенный RAG отвечает
 * `disabled` раньше `bad_scope`. */
bool begin(const RagCallContext& ctx, const std::string& requested,
           RequiredPermission required, std::string& suffix,
           char** out_error) {
    if (!ctx.allowed) {
        *out_error = error_json("disabled",
                                "RAG is disabled in settings");
        return false;
    }
    if (!ctx.registry) {
        *out_error = error_json("unavailable",
                                "RAG registry is not configured");
        return false;
    }
    if (!permission_granted(ctx, required)) {
        *out_error = error_json(
            "denied", std::string("plugin lacks permission '") +
                          permission_name(required) + "'");
        return false;
    }
    std::string error;
    if (!resolve_scope(ctx.plugin_name, requested, suffix, error)) {
        *out_error = error_json("bad_scope", error);
        return false;
    }
    return true;
}

/* Стор для плагина или готовый отказ. */
std::shared_ptr<core::RagStore> store_for(const RagCallContext& ctx,
                                          const std::string& suffix,
                                          char** out_error) {
    std::string error;
    auto store = ctx.registry->get(ctx.plugin_name, suffix, &error);
    if (!store) {
        /* Реестр отказывает только на невалидном имени — но имя уже
         * проверено в resolve_scope, поэтому сюда попадают лишь
         * внутренние ошибки (не создался каталог и т.п.). */
        *out_error = error_json("unavailable",
                                "cannot open scope: " + error);
        return nullptr;
    }
    return store;
}

json chunk_to_json(const core::RagChunk& c) {
    json j;
    j["content"] = c.content;
    j["document_id"] = c.document_id;
    j["chunk_index"] = c.chunk_index;
    j["file_path"] = c.file_path;
    j["symbol_name"] = c.symbol_name;
    j["start_line"] = c.start_line;
    j["end_line"] = c.end_line;
    /* score здесь нет намеренно: search_hybrid() сливает keyword- и
     * вектор-выдачу и релевантность не считает. Ноль «потому что так
     * надо» был бы хуже отсутствующего поля — плагин начал бы сортировать
     * по нему. Поле появится вместе с ранжированием (отдельная задача). */
    return j;
}

} // namespace

bool plugin_has_permission(const std::vector<std::string>& permissions,
                           const std::string& required) {
    /* Пустое `required` — не право, а ошибка вызывающего: «разреши всем»
     * через пустую строку было бы тихой дырой. Считаем, что не выдано. */
    if (required.empty()) return false;
    for (const auto& p : permissions) {
        if (p == required) return true;
    }
    return false;
}

bool resolve_scope(const std::string& plugin_name,
                   const std::string& requested,
                   std::string& suffix_out,
                   std::string& error) {
    /* Выходной параметр — наш: иначе вызывающий, забывший очистить строку,
     * получил бы «суффикс от предыдущего вызова» и проиндексировал не туда. */
    suffix_out.clear();

    const std::string prefix = "plugin:" + plugin_name;

    if (requested.empty()) {
        /* Скоуп по умолчанию: имя плагина, без суффикса. */
    } else {
        if (requested.compare(0, prefix.size(), prefix) != 0) {
            error = "scope '" + requested + "' is not a scope of plugin '" +
                    plugin_name + "'";
            return false;
        }
        const std::string rest = requested.substr(prefix.size());
        if (!rest.empty()) {
            if (rest[0] != ':') {
                /* `plugin:wp_coder_extra` — не «другой суффикс», а попытка
                 * назваться чужим именем с приставкой. */
                error = "scope '" + requested + "' is not a scope of plugin '" +
                        plugin_name + "'";
                return false;
            }
            if (rest.size() == 1) {
                /* «plugin:wp_coder:» с пустым суффиксом — это опечатка, а
                 * не скоуп по умолчанию. Молча принять её значило бы
                 * проиндексировать не туда, куда просил плагин. */
                error = "empty scope suffix in '" + requested + "'";
                return false;
            }
            suffix_out = rest.substr(1);
        }
    }

    /* Полное имя собирает реестр — здесь только проверка. */
    std::string normalized;
    std::string norm_error;
    if (!core::normalize_scope(plugin_name, suffix_out, normalized, &norm_error)) {
        error = norm_error;
        return false;
    }
    return true;
}

char* rag_search_ex_impl(const RagCallContext& ctx,
                         const std::string& requested_scope,
                         const std::string& query, int k,
                         const std::string& path_filter) {
    std::string suffix;
    char* err = nullptr;
    if (!begin(ctx, requested_scope, RequiredPermission::Access, suffix, &err))
        return err;

    if (query.empty()) return error_json("bad_request", "empty query");
    if (k <= 0) return error_json("bad_request", "k must be positive");

    auto store = store_for(ctx, suffix, &err);
    if (!store) return err;

    const auto chunks = store->search(query, k, path_filter);

    json j;
    j["ok"] = true;
    j["scope"] = store->scope();
    j["chunks"] = json::array();
    for (const auto& c : chunks) {
        j["chunks"].push_back(chunk_to_json(c));
    }
    return dup_string(j.dump());
}

char* rag_index_document_ex_impl(const RagCallContext& ctx,
                                 const std::string& requested_scope,
                                 const std::string& path,
                                 const std::string& opts_json) {
    std::string suffix;
    char* err = nullptr;
    if (!begin(ctx, requested_scope, RequiredPermission::Write, suffix, &err))
        return err;

    if (path.empty()) return error_json("bad_request", "empty path");

    /* opts_json зарезервирован под R4.1 (max_bytes/max_chunks/deadline_ms/
     * reindex). Сейчас он принимается и игнорируется: так плагин,
     * написанный под R4, не получит «bad_request» от хоста R2, а поведение
     * останется прежним (индексация целиком). */
    (void)opts_json;

    auto store = store_for(ctx, suffix, &err);
    if (!store) return err;

    const int chunks = store->index_document(path);
    const core::RagStoreInfo info = store->info();

    json j;
    j["ok"] = true;
    j["scope"] = store->scope();
    j["chunks"] = chunks;
    j["documents"] = info.documents;
    j["truncated"] = false;   /* квоты — R4.2 */
    j["reason"] = "";
    return dup_string(j.dump());
}

char* rag_scope_info_impl(const RagCallContext& ctx,
                          const std::string& requested_scope) {
    std::string suffix;
    char* err = nullptr;
    if (!begin(ctx, requested_scope, RequiredPermission::Access, suffix, &err))
        return err;

    auto store = store_for(ctx, suffix, &err);
    if (!store) return err;

    const core::RagStoreInfo info = store->info();

    json j;
    j["ok"] = true;
    j["scope"] = info.scope;
    j["documents"] = info.documents;
    j["chunks"] = info.chunks;
    j["bytes"] = info.bytes;
    j["embeddings"] = info.embeddings;
    j["model"] = info.model;
    j["ready"] = info.ready;
    j["reason"] = info.reason;
    return dup_string(j.dump());
}

char* rag_scope_remove_document_impl(const RagCallContext& ctx,
                                     const std::string& requested_scope,
                                     const std::string& path) {
    std::string suffix;
    char* err = nullptr;
    if (!begin(ctx, requested_scope, RequiredPermission::Write, suffix, &err))
        return err;

    if (path.empty()) return error_json("bad_request", "empty path");

    auto store = store_for(ctx, suffix, &err);
    if (!store) return err;

    /* removed=false — не ошибка, а «этого документа в индексе и не было»:
     * удаление идемпотентно, как rm -f. Иначе плагин, узнавший о файле из
     * старого лога, не смог бы его убрать. */
    const bool removed = store->remove_document(path);

    json j;
    j["ok"] = true;
    j["scope"] = store->scope();
    j["removed"] = removed;
    return dup_string(j.dump());
}

int rag_scope_clear_impl(const RagCallContext& ctx,
                         const std::string& requested_scope) {
    std::string suffix;
    char* err = nullptr;
    if (!begin(ctx, requested_scope, RequiredPermission::Write, suffix, &err)) {
        std::free(err);
        return 0;
    }

    auto store = store_for(ctx, suffix, &err);
    if (!store) {
        std::free(err);
        return 0;
    }

    store->clear();
    return 1;
}

int rag_scope_flush_impl(const RagCallContext& ctx,
                         const std::string& requested_scope) {
    std::string suffix;
    char* err = nullptr;
    if (!begin(ctx, requested_scope, RequiredPermission::Access, suffix, &err)) {
        std::free(err);
        return 0;
    }

    auto store = store_for(ctx, suffix, &err);
    if (!store) {
        std::free(err);
        return 0;
    }

    return store->flush() ? 1 : 0;
}

} // namespace plugin
} // namespace llama_gui