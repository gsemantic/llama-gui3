#include "workspace_layout_manager.h"
#include "window_manager.h"
#include "../external/imgui/imgui.h"
#include "../external/imgui/imgui_internal.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <dirent.h>
#include <cstdio>
#include <unistd.h>

namespace llama_gui {
namespace ui {

bool WorkspaceLayoutManager::save(const std::string& name) {
    if (!window_manager_) return false;

    mkdir(workspaces_dir_.c_str(), 0755);

    // Обновляем позиции/размеры из реальных ImGui окон перед сохранением
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (g) {
        for (int i = 0; i < g->Windows.Size; i++) {
            ImGuiWindow* w = g->Windows[i];
            if (!w || w->Hidden) continue;
            std::string wname = w->Name;
            std::string wname_clean = wname;
            auto hash_pos = wname.find("##");
            if (hash_pos != std::string::npos) {
                wname_clean = wname.substr(0, hash_pos);
            }
            for (const auto& wm_name : window_manager_->getWindowNames()) {
                if (window_manager_->getImGuiName(wm_name) == wname_clean) {
                    // SetNextWindowSize() задаёт SizeFull — ТОТАЛЬНЫЙ размер окна (включая
                    // title bar, borders, padding), а НЕ content size.
                    // Поэтому сохраняем w->Size напрямую (total size), без вычитания decorations.
                    ImVec2 total_size = w->Size;
                    if (total_size.x < 100) total_size.x = 100;
                    if (total_size.y < 50) total_size.y = 50;
                    window_manager_->updateWindowPosition(wm_name, w->Pos);
                    window_manager_->updateWindowSize(wm_name, total_size);
                    break;
                }
            }
        }
    }

    WorkspaceConfig config = window_manager_->saveWorkspaceConfig(name);

    try {
        nlohmann::json config_json;
        config_json["name"] = config.name;

        nlohmann::json windows_json = nlohmann::json::array();
        for (const auto& window : config.windows) {
            // status_bar имеет NoResize|NoMove и не управляется WindowCoordinator —
            // его не нужно сохранять в workspace
            if (window.name == "status_bar") continue;
            nlohmann::json window_json;
            window_json["name"] = window.name;
            window_json["visible"] = window.visible;
            window_json["x"] = window.position.x;
            window_json["y"] = window.position.y;
            window_json["width"] = window.size.x;
            window_json["height"] = window.size.y;
            windows_json.push_back(window_json);
        }
        config_json["windows"] = windows_json;

        // Полное состояние ImGui (collapsed, скроллы, ширины колонок и пр.)
        // как ini-blob внутри JSON. Раскладка окон при этом остаётся в
        // "windows" выше — blob лишь дополняет её служебными состояниями.
        ImGuiContext* ctx = ImGui::GetCurrentContext();
        if (ctx) {
            size_t ini_size = 0;
            if (const char* ini_data = ImGui::SaveIniSettingsToMemory(&ini_size); ini_data && ini_size > 0) {
                config_json["imgui_ini"] = std::string(ini_data, ini_size);
            }

            // Z-order: стек фокуса корневых окон (задний → передний план).
            // Стандартный ini-blob порядок перекрытия НЕ сохраняет.
            nlohmann::json z_order_json = nlohmann::json::array();
            for (int i = 0; i < ctx->WindowsFocusOrder.Size; ++i) {
                ImGuiWindow* w = ctx->WindowsFocusOrder[i];
                if (!w || !w->Name) continue;
                std::string wname = w->Name;
                // Служебные/отладочные окна не восстанавливаем
                if (wname.rfind("##", 0) == 0 || wname.rfind("Debug", 0) == 0 ||
                    wname.rfind("Dear ImGui", 0) == 0) {
                    continue;
                }
                z_order_json.push_back(wname);
            }
            if (!z_order_json.empty()) {
                config_json["z_order"] = z_order_json;
            }
        }

        std::string filepath = workspaces_dir_ + "/" + name + ".json";
        std::ofstream file(filepath);
        file << config_json.dump(2);
        file.close();

        current_name_ = name;
        std::cout << "✓ Saved workspace: " << name << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to save workspace: " << e.what() << std::endl;
        return false;
    }
}

bool WorkspaceLayoutManager::load(const std::string& name) {
    if (!window_manager_) return false;

    std::string filepath = workspaces_dir_ + "/" + name + ".json";

    try {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            std::cerr << "Workspace file not found: " << filepath << std::endl;
            return false;
        }

        nlohmann::json config_json;
        file >> config_json;
        file.close();

        WorkspaceConfig config;
        config.name = config_json.value("name", name);

        if (config_json.contains("windows") && config_json["windows"].is_array()) {
            for (const auto& window_json : config_json["windows"]) {
                WindowState window_state;
                window_state.name = window_json.value("name", "");
                window_state.visible = window_json.value("visible", true);
                window_state.position = ImVec2(
                    window_json.value("x", 0),
                    window_json.value("y", 0)
                );
                window_state.size = ImVec2(
                    window_json.value("width", 800),
                    window_json.value("height", 600)
                );
                config.windows.push_back(window_state);
            }
        }

        window_manager_->loadWorkspaceConfig(config);

        // ini-blob ImGui: контекста при загрузке профиля ещё нет (load вызывается
        // из initialize() до run()) — откладываем применение до applyPendingImguiIni()
        pending_imgui_ini_.clear();
        if (config_json.contains("imgui_ini") && config_json["imgui_ini"].is_string()) {
            pending_imgui_ini_ = config_json["imgui_ini"].get<std::string>();
        }

        // Z-order применяется позже — окна должны быть созданы первыми кадрами
        pending_z_order_.clear();
        if (config_json.contains("z_order") && config_json["z_order"].is_array()) {
            for (const auto& z : config_json["z_order"]) {
                if (z.is_string()) pending_z_order_.push_back(z.get<std::string>());
            }
        }

        current_name_ = name;
        std::cout << "✓ Loaded workspace: " << name << " (" << config.windows.size() << " windows)" << std::endl;
        for (const auto& w : config.windows) {
            std::cout << "  " << w.name << ": pos=(" << w.position.x << "," << w.position.y
                      << ") size=(" << w.size.x << "," << w.size.y
                      << ") visible=" << w.visible << std::endl;
        }
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to load workspace: " << e.what() << std::endl;
        return false;
    }
}

bool WorkspaceLayoutManager::remove(const std::string& name) {
    std::string filepath = workspaces_dir_ + "/" + name + ".json";
    std::cout << "[WorkspaceLayoutManager] Removing: " << filepath << std::endl;
    int result = std::remove(filepath.c_str());
    std::cout << "[WorkspaceLayoutManager] remove result: " << result << std::endl;
    if (result == 0) {
        if (current_name_ == name) current_name_.clear();
        std::cout << "✓ Deleted workspace: " << name << std::endl;
        return true;
    }
    return false;
}

bool WorkspaceLayoutManager::rename(const std::string& old_name, const std::string& new_name) {
    std::string old_path = workspaces_dir_ + "/" + old_name + ".json";
    std::string new_path = workspaces_dir_ + "/" + new_name + ".json";

    if (std::rename(old_path.c_str(), new_path.c_str()) == 0) {
        if (current_name_ == old_name) current_name_ = new_name;
        std::cout << "✓ Renamed workspace: " << old_name << " → " << new_name << std::endl;
        return true;
    }
    return false;
}

std::vector<std::string> WorkspaceLayoutManager::list() const {
    std::vector<std::string> names;

    struct stat info;
    if (stat(workspaces_dir_.c_str(), &info) != 0) {
        return names;
    }

    DIR* dir = opendir(workspaces_dir_.c_str());
    if (!dir) return names;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string filename = entry->d_name;
        if (filename.size() > 5 && filename.substr(filename.size() - 5) == ".json") {
            names.push_back(filename.substr(0, filename.size() - 5));
        }
    }
    closedir(dir);
    return names;
}

bool WorkspaceLayoutManager::exists(const std::string& name) const {
    std::string filepath = workspaces_dir_ + "/" + name + ".json";
    struct stat info;
    return stat(filepath.c_str(), &info) == 0;
}

void WorkspaceLayoutManager::applyPendingImguiIni() {
    if (pending_imgui_ini_.empty()) return;
    if (!ImGui::GetCurrentContext()) {
        std::cerr << "[WorkspaceLayout] applyPendingImguiIni: контекст ImGui не создан" << std::endl;
        return;
    }
    ImGui::LoadIniSettingsFromMemory(pending_imgui_ini_.c_str(),
                                     pending_imgui_ini_.size());
    pending_imgui_ini_.clear();
    std::cout << "✓ Applied ImGui ini-blob from workspace profile" << std::endl;
}

void WorkspaceLayoutManager::tickDeferredApply() {
    if (pending_z_order_.empty()) return;

    ImGuiContext* ctx = ImGui::GetCurrentContext();
    // Окна создаются первыми Begin()-ами; даём два кадра на полный проход,
    // иначе FindWindowByName не найдёт ещё не созданные окна.
    if (!ctx || ImGui::GetFrameCount() < 2) return;

    size_t restored = 0;
    const size_t requested = pending_z_order_.size();
    // Фокусируем в порядке от заднего к переднему — верхним окажется
    // окно, которое было верхним при сохранении сессии.
    for (const std::string& name : pending_z_order_) {
        if (ImGuiWindow* w = ImGui::FindWindowByName(name.c_str())) {
            ImGui::FocusWindow(w);
            ++restored;
        }
    }
    pending_z_order_.clear();
    std::cout << "✓ Restored window z-order (" << restored
              << "/" << requested << " windows)" << std::endl;
}

} // namespace ui
} // namespace llama_gui
