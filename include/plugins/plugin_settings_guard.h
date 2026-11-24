#pragma once

/*
 * plugin_settings_guard.h — namespace-гард записи настроек (R3.2).
 *
 * ЗАЧЕМ. `host_settings_set` (plugin_manager.cpp) писал ЛЮБОЙ ключ в
 * `Settings::custom_settings_`, включая `plugin.<другой>.rag.enabled=true`:
 * плагин мог включить RAG за чужого или перезаписать чужие настройки. Ключи
 * `plugin.<name>.rag.*` принадлежат ХОСТУ (см. план §3.2/§3.4) — плагин
 * читает их, но не пишет.
 *
 * ПРАВИЛО (одно, §3.4):
 *   - ключ ВНЕ namespace `plugin.`          → можно (обычные настройки);
 *   - ключ `plugin.<self>.X`, где X ≠ `rag` → можно (своё);
 *   - иначе (`plugin.<чужой>.…`, `plugin.<self>.rag…`) → нельзя.
 *
 * Почему отдельный файл: та же причина, что у plugin_rag_gate.cpp и
 * plugin_rag_api.cpp — `host_settings_set` в анонимном namespace, и тест
 * его не видит. Здесь чистая функция, проверяемая без PluginManager.
 */

#include <string>

namespace llama_gui {
namespace plugin {

enum class SettingsWriteDecision { Allowed, Denied };

/* Может ли плагин с (уже санитизированным) именем `self` записать `key`
 * через host_settings_set. `self` пуст (нет плагина) → любой `plugin.*`
 * запрещён: доступ к чужим настройкам не выдаётся по умолчанию. */
SettingsWriteDecision check_plugin_settings_write(const std::string& self,
                                                  const std::string& key);

} // namespace plugin
} // namespace llama_gui