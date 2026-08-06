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

    // Для отслеживания: позиция окна на предыдущем кадре
    std::unordered_map<std::string, ImVec2> prev_positions_;
};

} // namespace ui
} // namespace llama_gui
