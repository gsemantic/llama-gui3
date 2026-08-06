#include "window_coordinator.h"
#include "workspace_applier.h"
#include "window_manager.h"
#include "../external/imgui/imgui.h"
#include "../external/imgui/imgui_internal.h"

namespace llama_gui {
namespace ui {

void WindowCoordinator::registerWindow(const std::string& name, RenderCallback callback,
                                       bool render_always, const std::string& imgui_name,
                                       bool* p_open) {
    windows_.emplace_back(name, WindowEntry{std::move(callback), render_always, imgui_name, p_open});
}

void WindowCoordinator::bringToFront(const std::string& name) {
    for (const auto& n : focus_requests_) {
        if (n == name) return;
    }
    focus_requests_.push_back(name);
}

void WindowCoordinator::renderAll() {
    bool want_apply = workspace_applier_ && workspace_applier_->hasPendingPositions();
    bool any_applied = false;

    // Get display size for dock calculations
    ImVec2 display_size = ImGui::GetIO().DisplaySize;

    // Единая фаза: SetNextWindowSize ПЕРЕД каждым Begin(), позиция ПОСЛЕ.
    // Раньше SetNextWindowSize вызывался пакетом для всех окон ДО любого Begin(),
    // но ImGui хранит NextWindowData в одном слоте — каждый вызов перезаписывал
    // предыдущий, и только последнее окно получало корректный размер.
    for (auto& [name, entry] : windows_) {
        if (entry.render_always || (window_manager_ && window_manager_->isWindowVisible(name))) {
            // Check if window is docked — force position and size
            if (window_manager_ && window_manager_->isWindowDocked(name)) {
                DockPosition dock_pos = window_manager_->getWindowDockPosition(name);
                ImVec2 docked_pos, docked_size;

                switch (dock_pos) {
                    case DockPosition::Left:
                        docked_pos = ImVec2(0, 0);
                        docked_size = ImVec2(display_size.x * 0.5f, display_size.y);
                        break;
                    case DockPosition::Right:
                        docked_pos = ImVec2(display_size.x * 0.5f, 0);
                        docked_size = ImVec2(display_size.x * 0.5f, display_size.y);
                        break;
                    case DockPosition::Top:
                        docked_pos = ImVec2(0, 0);
                        docked_size = ImVec2(display_size.x, display_size.y * 0.5f);
                        break;
                    case DockPosition::Bottom:
                        docked_pos = ImVec2(0, display_size.y * 0.5f);
                        docked_size = ImVec2(display_size.x, display_size.y * 0.5f);
                        break;
                    case DockPosition::Fullscreen:
                        docked_pos = ImVec2(0, 0);
                        docked_size = display_size;
                        break;
                    default:
                        docked_pos = ImVec2(0, 0);
                        docked_size = ImVec2(400, 300);
                        break;
                }

                ImGui::SetNextWindowPos(docked_pos, ImGuiCond_Always);
                ImGui::SetNextWindowSize(docked_size, ImGuiCond_Always);
            }
            // SetNextWindowSize ПЕРЕД Begin() — для конкретного этого окна
            else if (want_apply && !entry.imgui_name.empty()) {
                ImVec2 pos, size;
                if (workspace_applier_->getPosition(name, pos, size)) {
                    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
                }
            }

            // Одноразовый bring-to-front: вызываем SetNextWindowFocus ПЕРЕД Begin(),
            // чтобы окно перерисовалось поверх остальных (порядок отрисовки ImGui
            // определяется фокусом, а не порядком Begin()).
            bool should_focus = false;
            for (auto it = focus_requests_.begin(); it != focus_requests_.end(); ++it) {
                if (*it == name) {
                    should_focus = true;
                    focus_requests_.erase(it);
                    break;
                }
            }
            if (should_focus) {
                ImGui::SetNextWindowFocus();
            }

            if (entry.callback) {
                entry.callback();
            }

            // Если окно закрыто через крестик ImGui — синхронизируем WindowManager
            if (entry.p_open && !*entry.p_open && window_manager_ && window_manager_->isWindowVisible(name)) {
                window_manager_->setWindowVisible(name, false);
            }

            if (entry.imgui_name.empty() || !window_manager_) continue;

            ImGuiContext* g = ImGui::GetCurrentContext();
            if (!g) continue;

            for (int i = 0; i < g->Windows.Size; i++) {
                ImGuiWindow* w = g->Windows[i];
                if (!w || w->Hidden) continue;
                std::string wname = w->Name;
                auto hash_pos = wname.find("##");
                if (hash_pos != std::string::npos) wname = wname.substr(0, hash_pos);
                if (wname != entry.imgui_name) continue;

                // Позиция — после Begin()
                if (want_apply) {
                    ImVec2 pos, size;
                    if (workspace_applier_->getPosition(name, pos, size)) {
                        w->Pos = pos;
                        any_applied = true;
                    }
                }

                // Примагничивание во время движения/ресайза.
                // Для перемещения — прилипание позиции к сетке с порогом.
                // Для ресайза — примагничиваем размер, сохраняя противоположный
                // край неподвижным. Иначе при перетаскивании левого/верхнего края
                // окно "расползается" в обе стороны (правый/нижний край дрейфует).
                auto& gs = window_manager_->getGridSnappingSystem();
                if (gs.isEnabled()) {
                    const auto& gs_settings = gs.getSettings();
                    auto it = prev_states_.find(name);
                    if (it != prev_states_.end()) {
                        const PrevWindowState& prev = it->second;
                        bool pos_changed_x = (w->Pos.x != prev.pos.x);
                        bool pos_changed_y = (w->Pos.y != prev.pos.y);
                        bool size_changed_x = (w->Size.x != prev.size.x);
                        bool size_changed_y = (w->Size.y != prev.size.y);
                        bool size_changed = size_changed_x || size_changed_y;

                        if (size_changed && gs_settings.snap_size) {
                            // Ресайз: примагничиваем размер, фиксируя неподвижный край.
                            // Если позиция по оси изменилась — край был "притянут" к мыши
                            // (левый/верхний), значит фиксируем противоположный (правый/нижний).
                            ImVec2 snapped = gs.snapSize(w->Size);
                            if (snapped.x != w->Size.x) {
                                if (pos_changed_x) {
                                    float right_edge = w->Pos.x + w->Size.x;
                                    w->Size.x = snapped.x;
                                    w->Pos.x = right_edge - snapped.x;
                                } else {
                                    w->Size.x = snapped.x;
                                }
                            }
                            if (snapped.y != w->Size.y) {
                                if (pos_changed_y) {
                                    float bottom_edge = w->Pos.y + w->Size.y;
                                    w->Size.y = snapped.y;
                                    w->Pos.y = bottom_edge - snapped.y;
                                } else {
                                    w->Size.y = snapped.y;
                                }
                            }
                            window_manager_->setWindowSizeRaw(name, w->Size);
                            window_manager_->setWindowPositionRaw(name, w->Pos);
                        } else if (!size_changed && (pos_changed_x || pos_changed_y) && gs_settings.snap_position) {
                            // Перемещение окна
                            ImVec2 snapped = gs.snapPositionWithThreshold(w->Pos, gs_settings.snap_threshold);
                            if (snapped.x != w->Pos.x || snapped.y != w->Pos.y) {
                                w->Pos = snapped;
                            }
                            window_manager_->setWindowPositionRaw(name, w->Pos);
                        }
                    }
                    prev_states_[name] = { w->Pos, w->Size };
                } else {
                    prev_states_.erase(name);
                }
                break;
            }
        }
    }

    if (any_applied && workspace_applier_) {
        workspace_applier_->confirmApplied();
    }
}

bool WindowCoordinator::renderDockContextMenu(const std::string& window_name) {
    return renderDockMenuStatic(window_name, window_manager_);
}

bool WindowCoordinator::renderDockMenuStatic(const std::string& window_name, WindowManager* wm) {
    if (!wm) return false;

    bool acted = false;
    bool is_docked = wm->isWindowDocked(window_name);

    if (ImGui::BeginPopupContextItem(("##dock_ctx_" + window_name).c_str())) {
        if (is_docked) {
            if (ImGui::MenuItem("Undock")) {
                wm->undockWindow(window_name);
                acted = true;
            }
        } else {
            if (ImGui::MenuItem("Dock Left")) {
                wm->dockWindow(window_name, DockPosition::Left);
                acted = true;
            }
            if (ImGui::MenuItem("Dock Right")) {
                wm->dockWindow(window_name, DockPosition::Right);
                acted = true;
            }
            if (ImGui::MenuItem("Dock Top")) {
                wm->dockWindow(window_name, DockPosition::Top);
                acted = true;
            }
            if (ImGui::MenuItem("Dock Bottom")) {
                wm->dockWindow(window_name, DockPosition::Bottom);
                acted = true;
            }
            if (ImGui::MenuItem("Dock Fullscreen")) {
                wm->dockWindow(window_name, DockPosition::Fullscreen);
                acted = true;
            }
        }

        ImGui::EndPopup();
    }

    return acted;
}

} // namespace ui
} // namespace llama_gui
