#pragma once

#include "grid_snapping_system.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

#ifdef USE_IMGUI
#include "../external/imgui/imgui.h"
#endif

namespace llama_gui {
namespace ui {

// Dock positions
enum class DockPosition {
    None = 0,
    Left,       // Left half of screen
    Right,      // Right half of screen
    Top,        // Top third of screen
    Bottom,     // Bottom third of screen
    Fullscreen  // Fill entire screen
};

struct WindowState {
    std::string name;
    bool visible = true;
    ImVec2 position = ImVec2(0, 0);
    ImVec2 size = ImVec2(0, 0);
    bool docked = false;
    DockPosition dock_position = DockPosition::None;
    ImVec2 pre_dock_position = ImVec2(0, 0);  // Saved position before docking
    ImVec2 pre_dock_size = ImVec2(0, 0);      // Saved size before docking
    bool snapped_to_grid = true;
};

struct WorkspaceConfig {
    std::string name;
    std::vector<WindowState> windows;
    std::string theme;
    std::unordered_map<std::string, std::string> settings;
    std::string menu_config;
};

class WindowManager {
public:
    WindowManager() = default;
    ~WindowManager() = default;

    void addWindow(const std::string& name, bool visible = true,
                   const ImVec2& position = ImVec2(0, 0),
                   const ImVec2& size = ImVec2(0, 0));

    /**
     * @brief Зарегистрировать ImGui имя для окна (для сохранения позиций)
     * @param wm_name Имя в WindowManager (например "chat")
     * @param imgui_name Имя в ImGui::Begin (например "Chat")
     */
    void setImGuiName(const std::string& wm_name, const std::string& imgui_name);

    void removeWindow(const std::string& name);
    void toggleWindow(const std::string& name);
    void setWindowVisible(const std::string& name, bool visible);
    bool isWindowVisible(const std::string& name) const;
    void updateWindowPosition(const std::string& name, const ImVec2& position, bool snap_to_grid = false);
    void updateWindowSize(const std::string& name, const ImVec2& size, bool snap_to_grid = false);
    ImVec2 getWindowPosition(const std::string& name) const;
    ImVec2 getWindowSize(const std::string& name) const;
    WindowState getWindowState(const std::string& name) const;
    std::string getImGuiName(const std::string& wm_name) const;
    std::vector<std::string> getWindowNames() const;
    std::vector<std::string> getAllWindowNames() const;
    std::vector<WindowState> getAllWindowStates() const;
    void loadWorkspaceConfig(const WorkspaceConfig& config);
    WorkspaceConfig saveWorkspaceConfig() const;
    WorkspaceConfig saveWorkspaceConfig(const std::string& name) const;

    // Dock/Undock support
    void dockWindow(const std::string& name, DockPosition position);
    void undockWindow(const std::string& name);
    bool isWindowDocked(const std::string& name) const;
    DockPosition getWindowDockPosition(const std::string& name) const;

    // Grid snapping
    void snapWindowToGrid(const std::string& name);
    void snapWindowSizeToGrid(const std::string& name);
    void snapAllWindowsToGrid();
    GridSnappingSystem& getGridSnappingSystem() { return grid_snapping_; }
    const GridSnappingSystem& getGridSnappingSystem() const { return grid_snapping_; }

    // Callbacks
    using WindowChangedCallback = std::function<void(const std::string&)>;
    void addWindowChangedCallback(WindowChangedCallback cb);
    void removeWindowChangedCallback(WindowChangedCallback cb);

private:
    void notifyWindowChanged(const std::string& name);
    std::unordered_map<std::string, WindowState> windows_;
    std::unordered_map<std::string, std::string> imgui_names_; // wm_name -> imgui_name
    std::vector<WindowChangedCallback> window_changed_callbacks_;
    GridSnappingSystem grid_snapping_;
};

} // namespace ui
} // namespace llama_gui
