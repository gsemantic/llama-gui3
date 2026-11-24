/*
 * plugin_settings_guard.cpp — namespace-гард записи настроек (R3.2).
 *
 * Обоснование и границы — в plugin_settings_guard.h. Здесь только код.
 */

#include "plugins/plugin_settings_guard.h"

namespace llama_gui {
namespace plugin {

SettingsWriteDecision check_plugin_settings_write(const std::string& self,
                                                  const std::string& key) {
    static const std::string kPluginNs = "plugin.";

    /* Не наша namespace — обычная настройка, пишем как раньше. */
    if (key.compare(0, kPluginNs.size(), kPluginNs) != 0) {
        return SettingsWriteDecision::Allowed;
    }

    /* Внутри `plugin.` — только свой `plugin.<self>.`, и не `.rag.`.
     * Пустой self не совпадает ни с одним префиксом → Denied. */
    const std::string own_prefix = kPluginNs + self + ".";
    if (self.empty() ||
        key.compare(0, own_prefix.size(), own_prefix) != 0) {
        return SettingsWriteDecision::Denied;
    }

    /* Первый сегмент после `plugin.<self>.` — `rag` → host-owned. */
    const std::string rest = key.substr(own_prefix.size());
    const std::string first = rest.substr(0, rest.find('.'));
    if (first == "rag") {
        return SettingsWriteDecision::Denied;
    }

    return SettingsWriteDecision::Allowed;
}

} // namespace plugin
} // namespace llama_gui