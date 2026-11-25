#pragma once

/*
 * plugin_rag_dialog.h — R3.5: интерактивный список плагинов с RAG-правами.
 *
 * В списке плагинов (команда open_plugins) — на каждый плагин:
 *   - чекбокс «RAG» (plugin.<name>.rag.enabled, читается plugin_rag_gate);
 *   - поле суффикса скоупа (какой скоуп показывать/чистить: "" — основной,
 *     "archive" — архив сессий и т.п.);
 *   - размер индекса (документы/чанки/байты из RagRegistry::info);
 *   - кнопка «Очистить» (RagRegistry::remove — rag_scope_clear).
 *
 * Диалог immediate-mode (ImGui), как RagSettingsDialog: MainWindow владеет
 * им и зовёт render() каждый кадр, когда он «виден». Доступ к плагинам и
 * реестру — через MainWindow (get_plugin_manager/get_rag_registry), чтобы
 * не хранить сырые указатели, живущие дольше самого диалога.
 */

namespace llama_gui {
namespace ui {

class MainWindow;  // forward declaration

class PluginRagDialog {
public:
    explicit PluginRagDialog(void* main_window = nullptr);
    ~PluginRagDialog() = default;

    void render();

    bool is_visible() const { return visible_; }
    void set_visible(bool visible) { visible_ = visible; }

private:
    void* main_window_ptr_ = nullptr;
    bool visible_ = false;
};

} // namespace ui
} // namespace llama_gui