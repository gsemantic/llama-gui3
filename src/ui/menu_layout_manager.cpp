#include "../include/ui/menu_layout_manager.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <sys/types.h>

namespace llama_gui {
namespace ui {

namespace {

// Создаёт все каталоги по пути файла (mkdir -p для префиксов)
void ensure_parent_dirs(const std::string& filepath) {
    size_t pos = 0;
    while ((pos = filepath.find('/', pos + 1)) != std::string::npos) {
        mkdir(filepath.substr(0, pos).c_str(), 0755);
    }
}

void to_json(nlohmann::json& j, const MenuLayoutEntry& e) {
    j["is_group"] = e.is_group;
    j["key"] = e.key;
}

void from_json_entry(const nlohmann::json& j, MenuLayoutEntry& e) {
    e.is_group = j.value("is_group", false);
    e.key = j.value("key", "");
}

void groups_to_json(nlohmann::json& menu_json, const std::vector<MenuLayoutGroup>& groups) {
    nlohmann::json groups_json = nlohmann::json::array();
    for (const auto& g : groups) {
        nlohmann::json g_json;
        g_json["id"] = g.id;
        g_json["title"] = g.title;
        nlohmann::json items_json = nlohmann::json::array();
        for (const auto& k : g.item_keys) {
            items_json.push_back(k);
        }
        g_json["items"] = items_json;
        groups_json.push_back(g_json);
    }
    menu_json["groups"] = groups_json;
}

void groups_from_json(const nlohmann::json& menu_json, std::vector<MenuLayoutGroup>& groups) {
    groups.clear();
    const nlohmann::json& groups_json = menu_json.value("groups", nlohmann::json::array());
    if (!groups_json.is_array()) return;
    for (const auto& g : groups_json) {
        MenuLayoutGroup group;
        group.id = g.value("id", "");
        group.title = g.value("title", "");
        const nlohmann::json& items_json = g.value("items", nlohmann::json::array());
        if (items_json.is_array()) {
            for (const auto& k : items_json) {
                if (k.is_string()) {
                    group.item_keys.push_back(k.get<std::string>());
                }
            }
        }
        if (!group.id.empty()) {
            groups.push_back(std::move(group));
        }
    }
}

} // namespace

// =========================================================================
// Workspace scope
// =========================================================================

void MenuLayoutManager::setActiveWorkspace(const std::string& name) {
    active_workspace_ = name.empty() ? "User" : name;
}

MenuLayoutForWorkspace& MenuLayoutManager::getOrCreateWorkspace(const std::string& name) {
    return workspaces_[name.empty() ? "User" : name];
}

MenuLayoutForWorkspace* MenuLayoutManager::findWorkspace(const std::string& name) {
    auto it = workspaces_.find(name.empty() ? "User" : name);
    return (it != workspaces_.end()) ? &it->second : nullptr;
}

const MenuLayoutForWorkspace* MenuLayoutManager::findWorkspace(const std::string& name) const {
    auto it = workspaces_.find(name.empty() ? "User" : name);
    return (it != workspaces_.end()) ? &it->second : nullptr;
}

// =========================================================================
// Загрузка / сохранение
// =========================================================================

bool MenuLayoutManager::load() {
    std::ifstream file(config_path_);
    if (!file.is_open()) {
        // Файла ещё нет — это нормальная ситуация для первого запуска
        return false;
    }

    try {
        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
        file.close();
        if (!deserializeFromJson(content)) {
            return false;
        }
        std::cout << "✓ Loaded menu layout: " << config_path_ << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to load menu layout: " << e.what() << std::endl;
        return false;
    }
}

bool MenuLayoutManager::save() const {
    ensure_parent_dirs(config_path_);

    try {
        std::ofstream file(config_path_);
        if (!file.is_open()) {
            std::cerr << "Failed to open menu layout for writing: " << config_path_ << std::endl;
            return false;
        }
        file << serializeToJson();
        file.close();

        dirty_ = false;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to save menu layout: " << e.what() << std::endl;
        return false;
    }
}

std::string MenuLayoutManager::serializeToJson() const {
    nlohmann::json workspaces_json = nlohmann::json::object();

    // Сортируем ключи для стабильного вида файла
    std::vector<std::string> ws_names;
    ws_names.reserve(workspaces_.size());
    for (const auto& [name, _] : workspaces_) {
        ws_names.push_back(name);
    }
    std::sort(ws_names.begin(), ws_names.end());

    for (const auto& ws_name : ws_names) {
        const auto& ws_layout = workspaces_.at(ws_name);
        nlohmann::json menus_json = nlohmann::json::object();

        std::vector<std::string> menu_keys;
        menu_keys.reserve(ws_layout.menus.size());
        for (const auto& [key, _] : ws_layout.menus) {
            menu_keys.push_back(key);
        }
        std::sort(menu_keys.begin(), menu_keys.end());

        for (const auto& menu_key : menu_keys) {
            const auto& menu_layout = ws_layout.menus.at(menu_key);
            nlohmann::json menu_json;
            menu_json["hidden"] = menu_layout.hidden;

            nlohmann::json hidden_json = nlohmann::json::array();
            for (const auto& h : menu_layout.hidden_items) {
                hidden_json.push_back(h);
            }
            menu_json["hidden_items"] = hidden_json;

            nlohmann::json entries_json = nlohmann::json::array();
            for (const auto& e : menu_layout.entries) {
                nlohmann::json e_json;
                to_json(e_json, e);
                entries_json.push_back(e_json);
            }
            menu_json["entries"] = entries_json;

            groups_to_json(menu_json, menu_layout.groups);

            menus_json[menu_key] = menu_json;
        }

        workspaces_json[ws_name]["menus"] = menus_json;
    }

    nlohmann::json root;
    root["version"] = 1;
    root["workspaces"] = workspaces_json;
    return root.dump(2);
}

bool MenuLayoutManager::deserializeFromJson(const std::string& json_str) {
    try {
        nlohmann::json root = nlohmann::json::parse(json_str);

        std::unordered_map<std::string, MenuLayoutForWorkspace> parsed;

        if (!root.contains("workspaces") || !root["workspaces"].is_object()) {
            return false;
        }

        for (auto it = root["workspaces"].begin(); it != root["workspaces"].end(); ++it) {
            MenuLayoutForWorkspace ws_layout;
            ws_layout.workspace_name = it.key();

            const nlohmann::json& menus_json = it.value().value("menus", nlohmann::json::object());
            if (!menus_json.is_object()) continue;

            for (auto mit = menus_json.begin(); mit != menus_json.end(); ++mit) {
                MenuLayoutForMenu menu_layout;
                menu_layout.hidden = mit.value().value("hidden", false);

                const nlohmann::json& hidden_json =
                    mit.value().value("hidden_items", nlohmann::json::array());
                if (hidden_json.is_array()) {
                    for (const auto& h : hidden_json) {
                        if (h.is_string()) {
                            menu_layout.hidden_items.push_back(h.get<std::string>());
                        }
                    }
                }

                const nlohmann::json& entries_json =
                    mit.value().value("entries", nlohmann::json::array());
                if (entries_json.is_array()) {
                    for (const auto& e : entries_json) {
                        MenuLayoutEntry entry;
                        from_json_entry(e, entry);
                        menu_layout.entries.push_back(entry);
                    }
                }

                groups_from_json(mit.value(), menu_layout.groups);

                ws_layout.menus[mit.key()] = std::move(menu_layout);
            }

            parsed[it.key()] = std::move(ws_layout);
        }

        workspaces_ = std::move(parsed);
        dirty_ = true;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to parse menu layout: " << e.what() << std::endl;
        return false;
    }
}

// =========================================================================
// Запросы
// =========================================================================

const MenuLayoutForWorkspace* MenuLayoutManager::findWorkspaceActive() const {
    return findWorkspace(active_workspace_);
}

const MenuLayoutForMenu* MenuLayoutManager::findMenuLayout(const std::string& menu_key) const {
    const auto* ws = findWorkspaceActive();
    if (!ws) return nullptr;
    auto it = ws->menus.find(menu_key);
    return (it != ws->menus.end()) ? &it->second : nullptr;
}

bool MenuLayoutManager::isMenuHidden(const std::string& menu_key) const {
    const auto* layout = findMenuLayout(menu_key);
    return layout && layout->hidden;
}

bool MenuLayoutManager::isItemHidden(const std::string& menu_key,
                                     const std::string& item_key) const {
    const auto* layout = findMenuLayout(menu_key);
    if (!layout || item_key.empty()) return false;
    return std::find(layout->hidden_items.begin(), layout->hidden_items.end(), item_key)
           != layout->hidden_items.end();
}

// =========================================================================
// Мутации
// =========================================================================

MenuLayoutForMenu& MenuLayoutManager::getOrCreateMenuLayout(const std::string& menu_key) {
    markDirty();
    return getOrCreateWorkspace(active_workspace_).menus[menu_key];
}

void MenuLayoutManager::setMenuHidden(const std::string& menu_key, bool hidden) {
    auto& layout = getOrCreateMenuLayout(menu_key);
    if (layout.hidden == hidden) return;
    layout.hidden = hidden;
    markDirty();
}

void MenuLayoutManager::setItemHidden(const std::string& menu_key,
                                      const std::string& item_key, bool hidden) {
    if (item_key.empty()) return;
    auto& layout = getOrCreateMenuLayout(menu_key);
    auto& list = layout.hidden_items;
    auto it = std::find(list.begin(), list.end(), item_key);
    if (hidden && it == list.end()) {
        list.push_back(item_key);
        markDirty();
    } else if (!hidden && it != list.end()) {
        list.erase(it);
        markDirty();
    }
}

std::string MenuLayoutManager::generateGroupId(const MenuLayoutForMenu& menu_layout) const {
    // Id уникален во всей конфигурации (все workspace/меню), чтобы избежать
    // коллизий при поиске групп между меню
    (void)menu_layout;
    int max_id = 0;
    for (const auto& [_, ws_layout] : workspaces_) {
        for (const auto& [__, menu] : ws_layout.menus) {
            for (const auto& g : menu.groups) {
                if (g.id.rfind("group_", 0) == 0) {
                    try {
                        max_id = std::max(max_id, std::stoi(g.id.substr(6)));
                    } catch (...) {
                        // Не числовой суффикс — игнорируем
                    }
                }
            }
        }
    }
    return "group_" + std::to_string(max_id + 1);
}

std::string MenuLayoutManager::createGroup(const std::string& menu_key, const std::string& title) {
    auto& layout = getOrCreateMenuLayout(menu_key);
    MenuLayoutGroup group;
    group.id = generateGroupId(layout);
    group.title = title.empty() ? group.id : title;

    // Группа сразу встаёт в конец порядка верхнего уровня
    MenuLayoutEntry entry;
    entry.is_group = true;
    entry.key = group.id;
    layout.entries.push_back(entry);
    layout.groups.push_back(std::move(group));
    markDirty();
    return layout.groups.back().id;
}

void MenuLayoutManager::removeGroup(const std::string& menu_key, const std::string& group_id) {
    auto* ws = findWorkspace(active_workspace_);
    if (!ws) return;
    auto it = ws->menus.find(menu_key);
    if (it == ws->menus.end()) return;

    auto& layout = it->second;
    auto& groups = layout.groups;
    groups.erase(std::remove_if(groups.begin(), groups.end(),
                                [&group_id](const MenuLayoutGroup& g) { return g.id == group_id; }),
                 groups.end());
    layout.entries.erase(std::remove_if(layout.entries.begin(), layout.entries.end(),
                                        [&group_id](const MenuLayoutEntry& e) {
                                            return e.is_group && e.key == group_id;
                                        }),
                         layout.entries.end());
    markDirty();
}

void MenuLayoutManager::setGroup(const std::string& menu_key, const MenuLayoutGroup& group) {
    if (group.id.empty()) return;
    auto& layout = getOrCreateMenuLayout(menu_key);
    for (auto& g : layout.groups) {
        if (g.id == group.id) {
            g.title = group.title;
            g.item_keys = group.item_keys;
            markDirty();
            return;
        }
    }
    layout.groups.push_back(group);
    markDirty();
}

bool MenuLayoutManager::moveItemToGroup(const std::string& menu_key, const std::string& group_id,
                                        const std::string& item_key) {
    if (item_key.empty()) return false;
    const MenuLayoutForWorkspace* ws = findWorkspace(active_workspace_);
    if (!ws) return false;
    auto ws_it = ws->menus.find(menu_key);
    if (ws_it == ws->menus.end()) return false;

    // Целевая группа должна существовать в указанном меню
    bool group_exists = false;
    for (const auto& g : ws_it->second.groups) {
        if (g.id == group_id) {
            group_exists = true;
            break;
        }
    }
    if (!group_exists) return false;

    auto* layout = &getOrCreateMenuLayout(menu_key);

    // Пункт не может состоять в двух группах одновременно
    for (auto& g : layout->groups) {
        auto& keys = g.item_keys;
        keys.erase(std::remove(keys.begin(), keys.end(), item_key), keys.end());
    }
    // И не должен оставаться на верхнем уровне
    layout->entries.erase(std::remove_if(layout->entries.begin(), layout->entries.end(),
                                         [&item_key](const MenuLayoutEntry& e) {
                                             return !e.is_group && e.key == item_key;
                                         }),
                          layout->entries.end());

    for (auto& g : layout->groups) {
        if (g.id == group_id) {
            g.item_keys.push_back(item_key);
            markDirty();
            return true;
        }
    }
    return false;
}

bool MenuLayoutManager::removeItemFromGroup(const std::string& menu_key,
                                            const std::string& group_id,
                                            const std::string& item_key) {
    auto* ws = findWorkspace(active_workspace_);
    if (!ws) return false;
    auto it = ws->menus.find(menu_key);
    if (it == ws->menus.end()) return false;

    auto& layout = it->second;
    for (auto& g : layout.groups) {
        if (g.id != group_id) continue;
        auto& keys = g.item_keys;
        auto kit = std::find(keys.begin(), keys.end(), item_key);
        if (kit == keys.end()) return false;
        keys.erase(kit);

        // Возвращаем пункт на верхний уровень в конец
        MenuLayoutEntry entry;
        entry.is_group = false;
        entry.key = item_key;
        layout.entries.push_back(entry);
        markDirty();
        return true;
    }
    return false;
}

void MenuLayoutManager::setEntries(const std::string& menu_key,
                                   const std::vector<MenuLayoutEntry>& entries) {
    auto& layout = getOrCreateMenuLayout(menu_key);
    layout.entries = entries;
    markDirty();
}

void MenuLayoutManager::resetMenu(const std::string& menu_key) {
    auto* ws = findWorkspace(active_workspace_);
    if (!ws) return;
    if (ws->menus.erase(menu_key) > 0) {
        markDirty();
    }
}

void MenuLayoutManager::resetActiveWorkspace() {
    const std::string ws_name = active_workspace_.empty() ? "User" : active_workspace_;
    if (workspaces_.erase(ws_name) > 0) {
        markDirty();
    }
}

void MenuLayoutManager::resetAll() {
    if (!workspaces_.empty()) {
        workspaces_.clear();
        markDirty();
    }
}

} // namespace ui
} // namespace llama_gui
