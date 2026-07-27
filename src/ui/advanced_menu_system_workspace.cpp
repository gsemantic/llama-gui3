#include "../include/ui/advanced_menu_system.h"
#include "../include/ui/window_manager.h"
#include "../external/imgui/imgui.h"
#include "../external/imgui/imgui_internal.h"
#include "../include/ui/localization_manager.h"
#include <algorithm>
#include <sstream>
#include <iostream>
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <nlohmann/json.hpp>

namespace llama_gui {
namespace ui {

std::string AdvancedMenuSystem::exportMenuConfiguration() const {
    nlohmann::json config;

    for (const auto& menu : menus_ordered_) {
        nlohmann::json menu_json;
        menu_json["name"] = menu->name;
        menu_json["enabled"] = menu->enabled;

        nlohmann::json items_json = nlohmann::json::array();
        for (const auto& item : menu->items) {
            nlohmann::json item_json;
            item_json["name"] = item.name;
            item_json["type"] = static_cast<int>(item.type);
            item_json["enabled"] = item.enabled;
            item_json["checked"] = item.checked;
            item_json["shortcut"] = item.shortcut;
            item_json["tooltip"] = item.tooltip;

            if (item.type == AdvancedMenuItemType::Submenu) {
                nlohmann::json submenu_json = nlohmann::json::array();
                for (const auto& subitem : item.submenu_items) {
                    nlohmann::json subitem_json;
                    subitem_json["name"] = subitem.name;
                    subitem_json["type"] = static_cast<int>(subitem.type);
                    subitem_json["enabled"] = subitem.enabled;
                    subitem_json["checked"] = subitem.checked;
                    subitem_json["shortcut"] = subitem.shortcut;
                    submenu_json.push_back(subitem_json);
                }
                item_json["submenu"] = submenu_json;
            }

            items_json.push_back(item_json);
        }

        menu_json["items"] = items_json;
        config.push_back(menu_json);
    }

    return config.dump(2);
}

bool AdvancedMenuSystem::importMenuConfiguration(const std::string& config) {
    try {
        nlohmann::json config_json = nlohmann::json::parse(config);

        if (!config_json.is_array()) {
            std::cerr << "Invalid configuration format" << std::endl;
            return false;
        }

        menus_ordered_.clear();
        menus_map_.clear();

        for (const auto& menu_json : config_json) {
            auto menu = std::make_unique<AdvancedMenu>();
            menu->name = menu_json.value("name", "");
            menu->enabled = menu_json.value("enabled", true);

            if (menu_json.contains("items") && menu_json["items"].is_array()) {
                for (const auto& item_json : menu_json["items"]) {
                    AdvancedMenuItem item;
                    item.name = item_json.value("name", "");
                    item.type = static_cast<AdvancedMenuItemType>(item_json.value("type", 0));
                    item.enabled = item_json.value("enabled", true);
                    item.checked = item_json.value("checked", false);
                    item.shortcut = item_json.value("shortcut", "");
                    item.tooltip = item_json.value("tooltip", "");

                    if (item_json.contains("submenu") && item_json["submenu"].is_array()) {
                        for (const auto& subitem_json : item_json["submenu"]) {
                            AdvancedMenuItem subitem;
                            subitem.name = subitem_json.value("name", "");
                            subitem.type = static_cast<AdvancedMenuItemType>(subitem_json.value("type", 0));
                            subitem.enabled = subitem_json.value("enabled", true);
                            subitem.checked = subitem_json.value("checked", false);
                            subitem.shortcut = subitem_json.value("shortcut", "");
                            item.submenu_items.push_back(subitem);
                        }
                    }

                    menu->items.push_back(item);
                }
            }

            menus_map_[menu->name] = menu.get();
            menus_ordered_.push_back(std::move(menu));
        }

        std::cout << "✓ Imported menu configuration" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to import configuration: " << e.what() << std::endl;
        return false;
    }
}

void AdvancedMenuSystem::saveWorkspace(const std::string& name) {
    // Create workspaces directory if it doesn't exist
    std::string workspaces_dir = ".config/llama-gui/workspaces";
    mkdir(workspaces_dir.c_str(), 0755);

    std::string filepath = workspaces_dir + "/" + name + ".json";

    // Обновляем позиции/размеры из реальных ImGui окон перед сохранением
    if (window_manager_) {
        ImGuiContext* g = ImGui::GetCurrentContext();
        if (g) {
            for (int i = 0; i < g->Windows.Size; i++) {
                ImGuiWindow* w = g->Windows[i];
                if (!w || w->Hidden) continue;
                std::string wname = w->Name;
                // Убираем ImGui суффикс ##N для сравнения
                std::string wname_clean = wname;
                auto hash_pos = wname.find("##");
                if (hash_pos != std::string::npos) {
                    wname_clean = wname.substr(0, hash_pos);
                }
                // Ищем соответствие через imgui_names_ mapping
                for (const auto& wm_name : window_manager_->getWindowNames()) {
                    if (window_manager_->getImGuiName(wm_name) == wname_clean) {
                        window_manager_->updateWindowPosition(wm_name, w->Pos);
                        window_manager_->updateWindowSize(wm_name, w->Size);
                        break;
                    }
                }
            }
        }
    }

    WorkspaceConfig config = window_manager_->saveWorkspaceConfig(name);
    // Не сохраняем конфигурацию меню - она управляется WorkspaceManager

    try {
        nlohmann::json config_json;
        config_json["name"] = config.name;

        // Serialize windows to JSON-compatible format
        nlohmann::json windows_json = nlohmann::json::array();
        for (const auto& window : config.windows) {
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
        // Не сохраняем menu_config

        std::ofstream file(filepath);
        file << config_json.dump(2);
        file.close();

        workspaces_[name] = config;
        std::cout << "✓ Saved workspace: " << name << " to " << filepath << std::endl;
        
        // Сохраняем конфигурацию WorkspaceManager
        if (workspace_manager_) {
            std::string workspace_manager_config_path = workspaces_dir + "/workspaces_config.json";
            workspace_manager_->saveConfig(workspace_manager_config_path);
            std::cout << "✓ Saved WorkspaceManager config to " << workspace_manager_config_path << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to save workspace: " << e.what() << std::endl;
    }
}

void AdvancedMenuSystem::loadWorkspace(const std::string& name) {
    // Всегда загружаем из файла для актуальных данных
    // Кэш в памяти может быть устаревшим

    std::string workspaces_dir = ".config/llama-gui/workspaces";
    std::string filepath = workspaces_dir + "/" + name + ".json";

    try {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            std::cerr << "Workspace file not found: " << filepath << std::endl;
            // Пробуем загрузить из кэша, если файл не найден
            auto it = workspaces_.find(name);
            if (it != workspaces_.end()) {
                setCurrentWorkspace(it->second);
                std::cout << "✓ Loaded workspace from cache: " << name << std::endl;
                return;
            }
            return;
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

        // Не загружаем menu_config - меню управляется WorkspaceManager

        workspaces_[name] = config;
        setCurrentWorkspace(config);
        std::cout << "✓ Loaded workspace: " << name << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Failed to load workspace: " << e.what() << std::endl;
    }
}

void AdvancedMenuSystem::setCurrentWorkspace(const WorkspaceConfig& config) {
    current_workspace_ = config;

    // Apply window configuration
    if (window_manager_) {
        window_manager_->loadWorkspaceConfig(config);
    }

    // Не применяем конфигурацию меню - меню управляется WorkspaceManager
}

std::vector<std::string> AdvancedMenuSystem::getWorkspaceNames() const {
    std::vector<std::string> names;
    for (const auto& pair : workspaces_) {
        names.push_back(pair.first);
    }
    return names;
}

bool AdvancedMenuSystem::hasWorkspace(const std::string& name) const {
    if (workspaces_.find(name) != workspaces_.end()) {
        return true;
    }
    // Также проверяем наличие файла на диске
    std::string filepath = ".config/llama-gui/workspaces/" + name + ".json";
    struct stat info;
    return stat(filepath.c_str(), &info) == 0;
}

void AdvancedMenuSystem::deleteWorkspace(const std::string& name) {
    auto it = workspaces_.find(name);
    if (it != workspaces_.end()) {
        workspaces_.erase(it);
        std::cout << "✓ Deleted workspace: " << name << std::endl;
    }

    // Also delete the file
    std::string workspaces_dir = ".config/llama-gui/workspaces";
    std::string filepath = workspaces_dir + "/" + name + ".json";
    std::remove(filepath.c_str());
}

std::vector<std::string> AdvancedMenuSystem::getAvailableWorkspaceFiles() const {
    std::vector<std::string> names;
    std::string workspaces_dir = ".config/llama-gui/workspaces";

    // Проверяем существование директории
    struct stat info;
    if (stat(workspaces_dir.c_str(), &info) != 0) {
        std::cout << "Workspace directory does not exist: " << workspaces_dir << std::endl;
        return names;
    }

    // Сканируем директорию на наличие .json файлов
    DIR* dir = opendir(workspaces_dir.c_str());
    if (dir == nullptr) {
        std::cerr << "Failed to open workspace directory: " << workspaces_dir << std::endl;
        return names;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string filename = entry->d_name;
        // Ищем файлы с расширением .json
        if (filename.size() > 5 && filename.substr(filename.size() - 5) == ".json") {
            // Убираем расширение .json
            std::string name = filename.substr(0, filename.size() - 5);
            names.push_back(name);
        }
    }

    closedir(dir);
    // Логирование удалено - метод вызывается слишком часто (каждый кадр)
    // std::cout << "Found " << names.size() << " workspace file(s) in " << workspaces_dir << std::endl;
    return names;
}

} // namespace ui
} // namespace llama_gui
