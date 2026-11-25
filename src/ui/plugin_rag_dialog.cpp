#include "../include/ui/plugin_rag_dialog.h"
#include "imgui.h"

#include "../include/ui/main_window.h"
#include "../include/core/settings.h"
#include "../include/core/rag_registry.h"
#include "../include/plugins/plugin_manager.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace llama_gui {
namespace ui {

PluginRagDialog::PluginRagDialog(void* main_window)
    : main_window_ptr_(main_window)
{
}

void PluginRagDialog::render() {
    if (!visible_) return;

    auto* mw = static_cast<MainWindow*>(main_window_ptr_);
    if (!mw) { visible_ = false; return; }

    llama_gui::core::Settings& settings = mw->get_settings();
    auto* pm = mw->get_plugin_manager();
    std::shared_ptr<llama_gui::core::RagRegistry> registry = mw->get_rag_registry();

    const auto plugins = pm ? pm->list_plugins()
                            : std::vector<plugin::PluginInfo>();

    if (ImGui::Begin("Плагины и RAG", &visible_)) {
        ImGui::Text("Плагины: %zu", plugins.size());
        ImGui::TextDisabled(
            "Чекбокс «RAG» — включён ли RAG плагину (plugin.<name>.rag.enabled). "
            "Суффикс выбирает скоуп: пусто — основной, «archive» — архив сессий. "
            "«Очистить» удаляет индекс выбранного скоупа.");
        ImGui::Separator();

        if (plugins.empty()) {
            ImGui::TextDisabled("Плагины не загружены.");
        }

        for (const auto& p : plugins) {
            ImGui::PushID(p.name.c_str());

            /* Заголовок плагина. */
            ImGui::Text("%s  v%s", p.name.c_str(),
                        p.version.empty() ? "?" : p.version.c_str());
            if (!p.description.empty())
                ImGui::TextDisabled("%s", p.description.c_str());

            /* Чекбокс «RAG»: plugin.<name>.rag.enabled (по умолчанию true). */
            const std::string enabled_key = "plugin." + p.name + ".rag.enabled";
            bool enabled = settings.get_custom_setting(enabled_key, "true") != "false";
            if (ImGui::Checkbox("RAG", &enabled))
                settings.set_custom_setting(enabled_key, enabled ? "true" : "false");

            /* Поле суффикса скоупа: буфер на плагин, инициализируется из
             * настроек один раз, затем пишется обратно при правке. */
            static std::map<std::string, std::array<char, 64>> buffers;
            auto it = buffers.find(p.name);
            if (it == buffers.end()) {
                std::array<char, 64> buf{};
                const std::string sfx =
                    settings.get_custom_setting("plugin." + p.name + ".rag.scope_suffix");
                std::snprintf(buf.data(), buf.size(), "%s", sfx.c_str());
                it = buffers.emplace(p.name, buf).first;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140);
            if (ImGui::InputText("##suffix", it->second.data(), it->second.size()))
                settings.set_custom_setting("plugin." + p.name + ".rag.scope_suffix",
                                            std::string(it->second.data()));
            ImGui::SameLine();
            ImGui::TextDisabled("суффикс скоупа");

            /* Размер индекса выбранного скоупа. */
            const std::string suffix(it->second.data());
            if (registry) {
                const llama_gui::core::RagStoreInfo info = registry->info(p.name, suffix);
                ImGui::Text("индекс: %zu док., %zu чанк., %zu байт%s",
                            info.documents, info.chunks, info.bytes,
                            info.ready ? "" : " (не готов)");
                ImGui::SameLine();
                if (ImGui::Button("Очистить"))
                    registry->remove(p.name, suffix);
            } else {
                ImGui::TextDisabled("реестр RAG недоступен");
            }

            ImGui::PopID();
            ImGui::Spacing();
        }
    }
    ImGui::End();
}

} // namespace ui
} // namespace llama_gui