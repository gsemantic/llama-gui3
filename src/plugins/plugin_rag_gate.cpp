/*
 * plugin_rag_gate.cpp — доступ плагинов к RAG (R0.2).
 *
 * Одна функция и одно правило: отвечать ли плагину на rag_*.
 *
 * ПОЧЕМУ ОТДЕЛЬНЫЙ ФАЙЛ, А НЕ plugin_manager.cpp. Функция объявлена в
 * plugin_manager.h и обязана иметь внешнюю линковку, иначе её не видит
 * тест. В plugin_manager.cpp все host_* лежат внутри анонимного
 * namespace, и определение там получило бы внутреннюю линковку:
 * `nm` показал бы `(anonymous namespace)::plugin_rag_allowed`, а линковка
 * теста — undefined reference. Та же причина, по которой разбор символов
 * живёт в ast_symbols.cpp, а не в plugin_manager.cpp: тестируемая логика
 * не должна зависеть от того, куда её положили в большом файле.
 *
 * ЧТО ИМЕННО ПРОВЕРЯЕТСЯ (обоснование — в plugin_manager.h):
 *   - менеджера RAG нет          → RAG выключен (отвечать нечем);
 *   - настроек нет               → RAG выключен («не знаю» = «нет», потому
 *     что «разрешаю» дало бы доступ к данным без решения пользователя);
 *   - settings->rag().enable_rag → единственный выключатель пользователя.
 *
 * До R0.2 каждый из пяти host_rag_* проверял только наличие rag_manager.
 * Пользователь выключал RAG в настройках — в собственном чате он
 * замолкал, а плагин с правом rag_access продолжал искать по документам.
 * Один переключатель с двумя поведениями: это два переключателя.
 */

#include "plugins/plugin_manager.h"

// Settings нужен полным типом: plugin_rag_allowed читает
// settings->rag().enable_rag, а в plugin_manager.h он только
// forward-объявлен. Без этого — «invalid use of incomplete type».
#include "core/settings.h"

#include <iostream>

namespace llama_gui {
namespace plugin {

bool plugin_rag_allowed(const PluginSubsystems& subsystems) {
    if (!subsystems.rag_manager) return false;
    if (!subsystems.settings) return false;
    return subsystems.settings->rag().enable_rag;
}

PluginRagConfig plugin_rag_config(const core::Settings* settings,
                                  const std::string& plugin_name) {
    PluginRagConfig cfg;   /* enabled = true по умолчанию (§3.2) */
    if (!settings || plugin_name.empty()) return cfg;

    const std::string key = "plugin." + plugin_name + ".rag.enabled";
    if (settings->has_custom_setting(key)) {
        const std::string raw = settings->get_custom_setting(key);
        /* Значение приходит строкой (custom_settings_ — map<string,string>).
         * Принимаем "true"/"1" и "false"/"0"; опечатка не выключает RAG
         * (это удобство, а не граница безопасности — граница в правах R3.1),
         * но пишем в лог, чтобы «почему не выключилось» было видно. */
        if (raw == "true" || raw == "1") {
            cfg.enabled = true;
        } else if (raw == "false" || raw == "0") {
            cfg.enabled = false;
        } else {
            std::cerr << "[PluginManager] plugin '" << plugin_name
                      << "' has non-boolean rag.enabled='" << raw
                      << "' — treating as enabled" << std::endl;
        }
    }

    /* R4.1: суффикс скоупа (plugin.<name>.rag.scope_suffix). Пусто = не
     * задан. Читается на каждом вызове, как и enabled. */
    const std::string suffix_key = "plugin." + plugin_name + ".rag.scope_suffix";
    if (settings->has_custom_setting(suffix_key))
        cfg.scope_suffix = settings->get_custom_setting(suffix_key);

    return cfg;
}

} // namespace plugin
} // namespace llama_gui