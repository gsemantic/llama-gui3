#include "../include/ui/advanced_menu_system.h"
#include "../external/imgui/imgui.h"
#include <iostream>

namespace llama_gui {
namespace ui {

void WindowManager::addWindow(const std::string& name, bool visible,
                             const ImVec2& position, const ImVec2& size) {
    // Если окно уже зарегистрировано (например, его состояние уже загружено
    // из workspace ДО инициализации плагина), НЕ перезаписываем сохранённую
    // видимость/позицию/размер — иначе восстановление workspace не работает
    // для окон плагинов (host_window_register вызывает addWindow позже и
    // затирал бы загруженное состояние).
    auto it = windows_.find(name);
    if (it != windows_.end()) {
        it->second.snapped_to_grid = grid_snapping_.isEnabled();
        if (imgui_names_.find(name) == imgui_names_.end()) {
            imgui_names_[name] = name;
        }
        return;
    }
    WindowState state;
    state.name = name;
    state.visible = visible;
    state.position = position;
    state.size = size;
    state.snapped_to_grid = grid_snapping_.isEnabled();
    windows_[name] = state;
    // По умолчанию ImGui имя = WindowManager имя
    if (imgui_names_.find(name) == imgui_names_.end()) {
        imgui_names_[name] = name;
    }
    std::cout << "✓ Added window: " << name << " (visible: " << (visible ? "yes" : "no") << ")" << std::endl;
}

void WindowManager::setImGuiName(const std::string& wm_name, const std::string& imgui_name) {
    imgui_names_[wm_name] = imgui_name;
}

void WindowManager::removeWindow(const std::string& name) {
    auto it = windows_.find(name);
    if (it != windows_.end()) {
        windows_.erase(it);
        notifyWindowChanged(name);
        std::cout << "✓ Removed window: " << name << std::endl;
    }
}

void WindowManager::toggleWindow(const std::string& name) {
    auto it = windows_.find(name);
    if (it != windows_.end()) {
        it->second.visible = !it->second.visible;
        notifyWindowChanged(name);
        std::cout << "✓ Toggled window: " << name << " (visible: " << (it->second.visible ? "yes" : "no") << ")" << std::endl;
    }
}

void WindowManager::setWindowVisible(const std::string& name, bool visible) {
    auto it = windows_.find(name);
    if (it != windows_.end()) {
        it->second.visible = visible;
        notifyWindowChanged(name);
    }
}

bool WindowManager::isWindowVisible(const std::string& name) const {
    auto it = windows_.find(name);
    return (it != windows_.end()) ? it->second.visible : false;
}

void WindowManager::updateWindowPosition(const std::string& name, const ImVec2& position, bool snap_to_grid) {
    auto it = windows_.find(name);
    if (it != windows_.end()) {
        if (snap_to_grid || it->second.snapped_to_grid) {
            it->second.position = grid_snapping_.snapPosition(position);
            it->second.snapped_to_grid = true;
        } else {
            it->second.position = position;
            it->second.snapped_to_grid = false;
        }
        notifyWindowChanged(name);
    }
}

void WindowManager::updateWindowSize(const std::string& name, const ImVec2& size, bool snap_to_grid) {
    auto it = windows_.find(name);
    if (it != windows_.end()) {
        if (snap_to_grid || it->second.snapped_to_grid) {
            it->second.size = grid_snapping_.snapSize(size);
            it->second.snapped_to_grid = true;
        } else {
            it->second.size = size;
            it->second.snapped_to_grid = false;
        }
        notifyWindowChanged(name);
    }
}

void WindowManager::setWindowPositionRaw(const std::string& name, const ImVec2& position) {
    auto it = windows_.find(name);
    if (it != windows_.end()) {
        it->second.position = position;
        it->second.snapped_to_grid = false;
        notifyWindowChanged(name);
    }
}

void WindowManager::setWindowSizeRaw(const std::string& name, const ImVec2& size) {
    auto it = windows_.find(name);
    if (it != windows_.end()) {
        it->second.size = size;
        it->second.snapped_to_grid = false;
        notifyWindowChanged(name);
    }
}

ImVec2 WindowManager::getWindowPosition(const std::string& name) const {
    auto it = windows_.find(name);
    return (it != windows_.end()) ? it->second.position : ImVec2(0, 0);
}

ImVec2 WindowManager::getWindowSize(const std::string& name) const {
    auto it = windows_.find(name);
    return (it != windows_.end()) ? it->second.size : ImVec2(0, 0);
}

WindowState WindowManager::getWindowState(const std::string& name) const {
    auto it = windows_.find(name);
    return (it != windows_.end()) ? it->second : WindowState{};
}

std::string WindowManager::getImGuiName(const std::string& wm_name) const {
    auto it = imgui_names_.find(wm_name);
    return (it != imgui_names_.end()) ? it->second : wm_name;
}

std::vector<std::string> WindowManager::getWindowNames() const {
    std::vector<std::string> names;
    for (const auto& pair : windows_) {
        names.push_back(pair.first);
    }
    return names;
}

std::vector<std::string> WindowManager::getAllWindowNames() const {
    return getWindowNames();
}

std::vector<WindowState> WindowManager::getAllWindowStates() const {
    std::vector<WindowState> states;
    for (const auto& pair : windows_) {
        states.push_back(pair.second);
    }
    return states;
}

void WindowManager::loadWorkspaceConfig(const WorkspaceConfig& config) {
    for (const auto& window : config.windows) {
        windows_[window.name] = window;
    }
}

WorkspaceConfig WindowManager::saveWorkspaceConfig() const {
    WorkspaceConfig config;
    for (const auto& pair : windows_) {
        WindowState state;
        state.name = pair.second.name;
        state.visible = pair.second.visible;
        state.position = pair.second.position;
        state.size = pair.second.size;
        config.windows.push_back(state);
    }
    return config;
}

WorkspaceConfig WindowManager::saveWorkspaceConfig(const std::string& name) const {
    WorkspaceConfig config = saveWorkspaceConfig();
    config.name = name;
    return config;
}

// =========================================================================
// Grid Snapping (примагничивание по сетке)
// =========================================================================

void WindowManager::snapWindowToGrid(const std::string& name) {
    auto it = windows_.find(name);
    if (it != windows_.end()) {
        it->second.position = grid_snapping_.snapPosition(it->second.position);
        it->second.snapped_to_grid = true;
        notifyWindowChanged(name);
        std::cout << "✓ Snapped window to grid: " << name << std::endl;
    }
}

void WindowManager::snapWindowSizeToGrid(const std::string& name) {
    auto it = windows_.find(name);
    if (it != windows_.end()) {
        it->second.size = grid_snapping_.snapSize(it->second.size);
        it->second.snapped_to_grid = true;
        notifyWindowChanged(name);
        std::cout << "✓ Snapped window size to grid: " << name << std::endl;
    }
}

void WindowManager::snapAllWindowsToGrid() {
    for (auto& pair : windows_) {
        if (pair.second.visible) {
            pair.second.position = grid_snapping_.snapPosition(pair.second.position);
            pair.second.size = grid_snapping_.snapSize(pair.second.size);
            pair.second.snapped_to_grid = true;
            notifyWindowChanged(pair.first);
        }
    }
    std::cout << "✓ Snapped all visible windows to grid" << std::endl;
}

// =========================================================================
// Callback методы для уведомлений об изменении окон
// =========================================================================

void WindowManager::addWindowChangedCallback(WindowChangedCallback callback) {
    if (callback) {
        window_changed_callbacks_.push_back(std::move(callback));
    }
}

void WindowManager::removeWindowChangedCallback(WindowChangedCallback callback) {
    window_changed_callbacks_.erase(
        std::remove_if(window_changed_callbacks_.begin(), window_changed_callbacks_.end(),
            [&callback](const WindowChangedCallback& cb) {
                return cb.target_type() == callback.target_type();
            }),
        window_changed_callbacks_.end()
    );
}

void WindowManager::notifyWindowChanged(const std::string& window_name) {
    for (const auto& callback : window_changed_callbacks_) {
        if (callback) {
            callback(window_name);
        }
    }
}

// =========================================================================
// Dock/Undock support
// =========================================================================

void WindowManager::dockWindow(const std::string& name, DockPosition position) {
    auto it = windows_.find(name);
    if (it == windows_.end()) return;

    auto& state = it->second;

    // Save current position/size before docking (only if not already docked)
    if (!state.docked) {
        state.pre_dock_position = state.position;
        state.pre_dock_size = state.size;
    }

    state.docked = true;
    state.dock_position = position;
    notifyWindowChanged(name);
    std::cout << "✓ Docked window: " << name << " at position " << static_cast<int>(position) << std::endl;
}

void WindowManager::undockWindow(const std::string& name) {
    auto it = windows_.find(name);
    if (it == windows_.end()) return;

    auto& state = it->second;

    // Restore saved position/size
    if (state.docked) {
        state.position = state.pre_dock_position;
        state.size = state.pre_dock_size;
    }

    state.docked = false;
    state.dock_position = DockPosition::None;
    notifyWindowChanged(name);
    std::cout << "✓ Undocked window: " << name << std::endl;
}

bool WindowManager::isWindowDocked(const std::string& name) const {
    auto it = windows_.find(name);
    return (it != windows_.end()) ? it->second.docked : false;
}

DockPosition WindowManager::getWindowDockPosition(const std::string& name) const {
    auto it = windows_.find(name);
    return (it != windows_.end()) ? it->second.dock_position : DockPosition::None;
}

} // namespace ui
} // namespace llama_gui
