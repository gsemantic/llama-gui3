// test_plugin_loader.cpp — интеграционный тест хоста системы плагинов.
//
// Создаёт PluginManager с пустыми подсистемами и проверяет, что:
//   - плагин из build/plugins/libhello_plugin.so загружается;
//   - его ll_plugin_init вызывается и возвращает 0 (host API не падает при
//     отсутствующих подсистемах);
//   - PluginManager корректно выгружает плагин (ll_plugin_shutdown + dlclose).

#include "plugins/plugin_manager.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef PLUGIN_TEST_DIR
#define PLUGIN_TEST_DIR "plugins"
#endif

int main() {
    llama_gui::plugin::PluginSubsystems subsystems;
    subsystems.config_dir = ".";
    subsystems.data_dir = ".";
    subsystems.plugins_dir = PLUGIN_TEST_DIR;

    llama_gui::plugin::PluginManager manager;

    if (!manager.initialize(subsystems)) {
        std::fprintf(stderr, "FAIL: initialize() вернул false\n");
        return 1;
    }

    auto plugins = manager.list_plugins();
    if (plugins.empty()) {
        std::fprintf(stderr, "FAIL: ни один плагин не загружен (искал в '%s')\n",
                     PLUGIN_TEST_DIR);
        return 1;
    }

    bool found_hello = false;
    bool manifest_ok = false;
    for (const auto& p : plugins) {
        std::printf("Loaded plugin: %s v%s (path: %s)\n",
                    p.name.c_str(), p.version.c_str(), p.path.c_str());
        if (p.name == "hello_plugin") {
            found_hello = true;
            manifest_ok = p.manifest.present && p.manifest.api_version == "1.0.0";
            if (p.manifest.present) {
                std::printf("  manifest: %s v%s (api %s), permissions: %zu\n",
                            p.manifest.name.c_str(), p.manifest.version.c_str(),
                            p.manifest.api_version.c_str(), p.manifest.permissions.size());
            }
        }
    }

    if (!found_hello) {
        std::fprintf(stderr, "FAIL: hello_plugin не найден\n");
        return 1;
    }
    if (!manifest_ok) {
        std::fprintf(stderr, "FAIL: манифест plugin.json не загружен или api_version != 1.0.0\n");
        return 1;
    }
    if (!manager.is_plugin_loaded("hello_plugin")) {
        std::fprintf(stderr, "FAIL: is_plugin_loaded('hello_plugin') == false\n");
        return 1;
    }

    // Пустое состояние — должно отработать без ошибок.
    manager.render_plugins();

    manager.shutdown();

    if (manager.is_plugin_loaded("hello_plugin")) {
        std::fprintf(stderr, "FAIL: плагин всё ещё загружен после shutdown()\n");
        return 1;
    }

    std::printf("PLUGIN LOADER TEST OK\n");
    return 0;
}
