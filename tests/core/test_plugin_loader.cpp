// test_plugin_loader.cpp — интеграционный тест хоста системы плагинов.
//
// Создаёт PluginManager с пустыми подсистемами и проверяет, что:
//   - плагин из build/plugins/libhello_plugin.so загружается;
//   - его ll_plugin_init вызывается и возвращает 0 (host API не падает при
//     отсутствующих подсистемах);
//   - PluginManager корректно выгружает плагин (ll_plugin_shutdown + dlclose).

#include "plugins/plugin_manager.h"
#include "ui/window_coordinator.h"
#include "imgui.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef PLUGIN_TEST_DIR
#define PLUGIN_TEST_DIR "plugins"
#endif

int main() {
    /* Плагины рисуют UI через ImGui; контекст нужен, как и в приложении. */
    ImGui::CreateContext();

    /* Координатор окон — как в приложении. С ним плагины с render_fn
     * регистрируются в нём (rendered_by_coordinator=true), и
     * render_plugins() НЕ зовёт их render_fn напрямую. Иначе render_fn ->
     * ImGui::Begin вне кадра -> SIGSEGV (OI-1, docs/OPEN_ISSUES.md). */
    llama_gui::ui::WindowCoordinator coordinator;

    llama_gui::plugin::PluginSubsystems subsystems;
    subsystems.config_dir = ".";
    subsystems.data_dir = ".";
    subsystems.plugins_dir = PLUGIN_TEST_DIR;
    subsystems.window_coordinator = &coordinator;

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
    bool foreign_manifest = false;
    for (const auto& p : plugins) {
        std::printf("Loaded plugin: %s v%s (path: %s)\n",
                    p.name.c_str(), p.version.c_str(), p.path.c_str());

        /* Инвариант: применённый манифест обязан принадлежать самому
         * плагину. Раньше это НЕ проверялось, и общий plugin.json в
         * build/plugins/ делал ровно обратное: hello_plugin получал
         * capabilities и версию wp_coder. Как только разрешения
         * начнут применяться (итерация И2), это дыра в правах. */
        if (p.manifest.present && !p.manifest.name.empty() &&
            p.manifest.name != p.name) {
            std::fprintf(stderr,
                         "FAIL: плагину '%s' подставлен чужой манифест '%s'\n",
                         p.name.c_str(), p.manifest.name.c_str());
            foreign_manifest = true;
        }

        if (p.name == "hello_plugin") {
            found_hello = true;
            /* У hello_plugin свой манифест (hello_plugin.json), значит
             * он обязан найтись — и именно СВОЙ. */
            manifest_ok = p.manifest.present &&
                          p.manifest.name == "hello_plugin" &&
                          p.manifest.api_version == "1.0.0";
            if (p.manifest.present) {
                std::printf("  manifest: %s v%s (api %s), permissions: %zu\n",
                            p.manifest.name.c_str(), p.manifest.version.c_str(),
                            p.manifest.api_version.c_str(), p.manifest.permissions.size());
            }
        }
    }

    if (foreign_manifest) return 1;
    if (!found_hello) {
        std::fprintf(stderr, "FAIL: hello_plugin не найден\n");
        return 1;
    }
    if (!manifest_ok) {
        std::fprintf(stderr,
                     "FAIL: у hello_plugin нет собственного манифеста "
                     "(name == 'hello_plugin', api 1.0.0)\n");
        return 1;
    }
    if (!manager.is_plugin_loaded("hello_plugin")) {
        std::fprintf(stderr, "FAIL: is_plugin_loaded('hello_plugin') == false\n");
        return 1;
    }

    /* R2.1: таблица ABI, которую получают плагины, обязана вмещать новые
     * поля RAG v2 (скоупы). Проверка не декоративная: хост пишет
     * `size = sizeof(LlamaHostApi)` при СВОЕЙ компиляции, и если он собран
     * с более старым plugin_api.h, размер окажется меньше — плагин с новым
     * полем читал бы за пределами заполненной структуры. Именно так
     * «разъезд»-ошибки так и выглядят: не падение, а мусор в указателе.
     *
     * Сравнение на РАВЕНСТВО, а не «>=»: тест и хост собираются из одного
     * дерева, и расхождение означает stale-сборку (ровно как макрос версии
     * плагина однажды остался от прошлой конфигурации). */
    {
        const LlamaHostApi& api = llama_gui::plugin::host_api_table();
        if (api.size != static_cast<uint32_t>(sizeof(LlamaHostApi))) {
            std::fprintf(stderr,
                         "FAIL: api.size=%u, sizeof(LlamaHostApi)=%zu — "
                         "хост собран с другим plugin_api.h\n",
                         api.size, sizeof(LlamaHostApi));
            return 1;
        }
        if (api.size < offsetof(LlamaHostApi, rag_index_cancel) + sizeof(void*)) {
            std::fprintf(stderr,
                         "FAIL: таблица ABI не вмещает поля RAG v2 "
                         "(нужно %zu байт, есть %u)\n",
                         offsetof(LlamaHostApi, rag_index_cancel) + sizeof(void*),
                         api.size);
            return 1;
        }
        /* R2.2: поля должны быть ЗАПОЛНЕНЫ, а не только умещаться.
         * Забытое присваивание в host_api_table выглядело бы как «поле
         * есть, но плагин получает nullptr» — и это прошло бы проверку
         * размера насквозь. */
        if (!api.rag_search_ex || !api.rag_index_document_ex ||
            !api.rag_scope_info || !api.rag_scope_remove_document ||
            !api.rag_scope_clear || !api.rag_scope_flush ||
            !api.rag_index_cancel) {
            std::fprintf(stderr,
                         "FAIL: часть полей RAG v2 не заполнена хостом\n");
            return 1;
        }
    }

    /* Пустое состояние — должно отработать без ошибок.
     *
     * Окна плагинов в этом тесте НЕ рисуются: в реальном приложении их
     * рисует WindowCoordinator, и тест эмулирует то же окружение
     * (координатор задан выше). Без координатора render_plugins() позвал бы
     * render_fn -> ImGui::Begin вне кадра -> SIGSEGV (OI-1,
     * docs/OPEN_ISSUES.md). Меню (если бы оно было) восстанавливается в
     * render_plugins() до вызова render_fn и от координатора не зависит. */
    manager.render_plugins();

    manager.shutdown();

    if (manager.is_plugin_loaded("hello_plugin")) {
        std::fprintf(stderr, "FAIL: плагин всё ещё загружен после shutdown()\n");
        return 1;
    }

    ImGui::DestroyContext();

    std::printf("PLUGIN LOADER TEST OK\n");
    return 0;
}
