#pragma once

#include <string>
#include <functional>
#include <vector>
#include <unordered_map>
#include "imgui.h"

namespace llama_gui {
namespace ui {

class WindowManager;
class WorkspaceApplier;

class WindowCoordinator {
public:
    using RenderCallback = std::function<void()>;

    WindowCoordinator() = default;
    ~WindowCoordinator() = default;

    void setWindowManager(WindowManager* manager) { window_manager_ = manager; }
    void setWorkspaceApplier(WorkspaceApplier* applier) { workspace_applier_ = applier; }

    void registerWindow(const std::string& name, RenderCallback callback,
                        bool render_always = false, const std::string& imgui_name = "",
                        bool* p_open = nullptr);

    /**
     * @brief Запросить фокус (bring-to-front) для окна при следующем рендере.
     *        Одноразовый запрос: применяется в ближайшем renderAll().
     * @param name Имя окна в WindowManager
     */
    void bringToFront(const std::string& name);

    /**
     * @brief Обновить ImGui-имя зарегистрированного окна (например, при смене языка,
     *        когда локализованный заголовок изменился). Иначе ImGui считает окно
     *        новым и сбрасывает его позицию/размер.
     * @param name Имя окна в WindowManager
     * @param new_imgui_name Новое ImGui-имя (локализованный заголовок)
     */
    void updateWindowImguiName(const std::string& name, const std::string& new_imgui_name);

    void renderAll();

    /**
     * @brief Render a dock context menu for a window (call from inside the window's Begin/End block)
     * @param window_name The WindowManager name of the window
     * @return true if a dock action was taken
     */
    bool renderDockContextMenu(const std::string& window_name);

    /**
     * @brief Render a dock context menu using a static helper (no WindowManager needed)
     *        Call this right after ImGui::Begin() to add right-click dock support
     * @param window_name The WindowManager name of the window
     * @param wm Pointer to WindowManager
     * @return true if a dock action was taken
     */
    static bool renderDockMenuStatic(const std::string& window_name, WindowManager* wm);

private:
    WindowManager* window_manager_ = nullptr;
    WorkspaceApplier* workspace_applier_ = nullptr;

    struct WindowEntry {
        RenderCallback callback;
        bool render_always;
        std::string imgui_name;
        bool* p_open = nullptr;
        bool show_dock_menu = false;  // Show dock context menu on right-click title bar
    };

    std::vector<std::pair<std::string, WindowEntry>> windows_;

    // Одноразовые запросы bring-to-front (применяются в ближайшем renderAll())
    std::vector<std::string> focus_requests_;

    // Для отслеживания: позиция и размер окна на предыдущем кадре
    struct PrevWindowState {
        ImVec2 pos{0, 0};
        ImVec2 size{0, 0};
    };
    std::unordered_map<std::string, PrevWindowState> prev_states_;
};

} // namespace ui
} // namespace llama_gui
