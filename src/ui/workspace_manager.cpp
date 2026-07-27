#include "../include/ui/workspace_manager.h"
#include "../include/core/logger.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <algorithm>

namespace llama_gui {
namespace ui {

using json = nlohmann::json;

// =========================================================================
// Конструктор/деструктор
// =========================================================================

WorkspaceManager::WorkspaceManager() = default;
WorkspaceManager::~WorkspaceManager() = default;

// =========================================================================
// Инициализация и конфигурация
// =========================================================================

void WorkspaceManager::initialize() {
    std::cout << "WorkspaceManager: Initializing with default configurations" << std::endl;

    // Создаем конфигурации по умолчанию
    workspaces_["User"] = createDefaultUserWorkspace();
    workspaces_["Developer"] = createDefaultDeveloperWorkspace();
    workspaces_["Admin"] = createDefaultAdminWorkspace();

    // Устанавливаем User workspace по умолчанию
    current_workspace_ = workspaces_["User"];

    std::cout << "✓ WorkspaceManager initialized with " << workspaces_.size() << " workspaces" << std::endl;
}

void WorkspaceManager::initializeWithConfig(const std::string& filepath) {
    // Сначала создаем конфигурации по умолчанию
    initialize();
    
    // Затем загружаем сохранённые конфигурации из файла (если есть)
    if (loadConfig(filepath)) {
        std::cout << "✓ WorkspaceManager: Loaded configuration from " << filepath << std::endl;
    } else {
        std::cout << "ℹ WorkspaceManager: No saved configuration found, using defaults" << std::endl;
    }
}

WorkspaceConfiguration WorkspaceManager::createDefaultUserWorkspace() const {
    WorkspaceConfiguration config;
    config.name = "User";
    config.type = WorkspaceType::User;
    config.show_developer_tools = false;
    config.show_admin_tools = false;

    // Видимые меню для User (используем постоянные ключи на английском)
    config.menu_configs = {
        {"File", "File", true, {}, {}},
        {"Settings", "Settings", true, {}, {}},
        {"View", "View", true, {}, {}},
        {"Agents", "Agents", true, {}, {}},
        {"Help", "Help", true, {}, {}}
    };

    // Доступные команды - только основные (быстрые) настройки
    config.enabled_commands = {
        "open_file", "save_file", "save_file_as",
        "toggle_window_chat", "toggle_window_files", "toggle_window_rag",
        "toggle_window_status_bar",
        "select_model", "model_directory",
        "toggle_window_agents", "rag", "search", "code", "summarize",
        "show_documentation", "show_shortcuts", "show_about",
        // Только быстрые настройки (Quick Settings)
        "open_settings_server", "open_settings_chat", "open_settings_models", "open_settings_ui", "open_settings_cloud",
        // Управление сервером (базовое)
        "server_start", "server_stop", "server_restart", "server_status"
    };

    // Скрытые команды - расширенные настройки (не будут отображаться в меню)
    config.disabled_commands = {
        // Расширенные настройки GPU и кэша
        "open_settings_gpu", "open_settings_cache",
        // Расширенные настройки сэмплирования и генерации
        "open_settings_sampling_basic", "open_settings_sampling_advanced", "open_settings_context", "open_settings_rope",
        // Расширенные настройки модели и сервера
        "open_settings_model_loading", "open_settings_batch", "open_settings_server_runtime", "open_settings_grammar", "open_settings_control_vectors",
        // Системные настройки
        "open_settings_logging", "open_settings_performance", "open_settings_advanced", "open_settings_output", "open_settings_tensor_override",
        // Отладка и метрики
        "show_metrics", "show_style_editor", "show_font_selector", "show_debug_log",
        "toggle_debug_mode", "show_command_manager_state", "show_window_manager_state",
        "export_debug_info", "clear_cache"
    };

    return config;
}

WorkspaceConfiguration WorkspaceManager::createDefaultDeveloperWorkspace() const {
    WorkspaceConfiguration config;
    config.name = "Developer";
    config.type = WorkspaceType::Developer;
    config.show_developer_tools = true;
    config.show_admin_tools = false;

    // Все меню видимы в Developer режиме (используем постоянные ключи на английском)
    config.menu_configs = {
        {"File", "File", true, {}, {}},
        {"Settings", "Settings", true, {}, {}},
        {"View", "View", true, {}, {}},
        {"Agents", "Agents", true, {}, {}},
        {"Developer", "Developer", true, {}, {}},
        {"Tools", "Tools", true, {}, {}},
        {"Help", "Help", true, {}, {}}
    };

    // Все команды доступны
    config.enabled_commands = {}; // пусто = все доступны
    config.disabled_commands = {};

    return config;
}

WorkspaceConfiguration WorkspaceManager::createDefaultAdminWorkspace() const {
    WorkspaceConfiguration config;
    config.name = "Admin";
    config.type = WorkspaceType::Admin;
    config.show_developer_tools = false;
    config.show_admin_tools = true;

    // Меню для администратора (используем постоянные ключи на английском)
    config.menu_configs = {
        {"File", "File", true, {}, {}},
        {"Settings", "Settings", true, {}, {}},
        {"View", "View", true, {}, {}},
        {"Performance", "Performance", true, {}, {}},
        {"Security", "Security", true, {}, {}},
        {"Logging", "Logging", true, {}, {}},
        {"Help", "Help", true, {}, {}}
    };

    // Скрываем некоторые пользовательские функции
    config.disabled_commands = {
        "show_metrics", "show_style_editor", "show_font_selector"
    };

    return config;
}

bool WorkspaceManager::loadConfig(const std::string& filepath) {
    try {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            std::cerr << "WorkspaceManager: Cannot open config file: " << filepath << std::endl;
            return false;
        }

        json json_data;
        file >> json_data;
        file.close();

        return deserializeFromJson(json_data.dump());
    } catch (const std::exception& e) {
        std::cerr << "WorkspaceManager: Failed to load config: " << e.what() << std::endl;
        return false;
    }
}

bool WorkspaceManager::saveConfig(const std::string& filepath) const {
    try {
        std::ofstream file(filepath);
        if (!file.is_open()) {
            std::cerr << "WorkspaceManager: Cannot create config file: " << filepath << std::endl;
            return false;
        }

        json json_data = json::parse(serializeToJson());
        file << json_data.dump(2);
        file.close();

        std::cout << "✓ WorkspaceManager: Saved config to " << filepath << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "WorkspaceManager: Failed to save config: " << e.what() << std::endl;
        return false;
    }
}

// =========================================================================
// Переключение рабочих пространств
// =========================================================================

void WorkspaceManager::switchWorkspace(WorkspaceType type) {
    std::string name = workspace_type_to_string(type);
    switchWorkspace(name);
}

bool WorkspaceManager::switchWorkspace(const std::string& name) {
    auto it = workspaces_.find(name);
    if (it == workspaces_.end()) {
        std::cerr << "WorkspaceManager: Workspace not found: " << name << std::endl;
        return false;
    }

    current_workspace_ = it->second;
    std::cout << "✓ WorkspaceManager: Switched to workspace '" << name << "'" << std::endl;

    notifyWorkspaceChanged();
    return true;
}

const WorkspaceConfiguration& WorkspaceManager::getCurrentWorkspace() const {
    return current_workspace_;
}

WorkspaceType WorkspaceManager::getCurrentWorkspaceType() const {
    return current_workspace_.type;
}

std::string WorkspaceManager::getCurrentWorkspaceName() const {
    return current_workspace_.name;
}

// =========================================================================
// Проверка видимости элементов
// =========================================================================

bool WorkspaceManager::isMenuVisible(const std::string& menu_key) const {
    for (const auto& config : current_workspace_.menu_configs) {
        if (config.menu_key == menu_key) {
            return config.visible;
        }
    }
    // Если меню не найдено в конфиге - скрываем его
    return false;
}

bool WorkspaceManager::isMenuItemVisible(const std::string& menu_name, const std::string& item_name) const {
    for (const auto& config : current_workspace_.menu_configs) {
        if (config.menu_name == menu_name) {
            // Если есть явный список видимых - проверяем его
            if (!config.visible_items.empty()) {
                return std::find(config.visible_items.begin(), config.visible_items.end(), item_name) 
                       != config.visible_items.end();
            }
            // Если есть явный список скрытых - проверяем его
            if (!config.hidden_items.empty()) {
                return std::find(config.hidden_items.begin(), config.hidden_items.end(), item_name) 
                       == config.hidden_items.end();
            }
            // Иначе видим
            return true;
        }
    }
    return true;
}

bool WorkspaceManager::isCommandEnabled(const std::string& command_name) const {
    // Если есть явный список отключенных команд
    if (!current_workspace_.disabled_commands.empty()) {
        return current_workspace_.disabled_commands.find(command_name) == current_workspace_.disabled_commands.end();
    }

    // Если есть явный список включенных команд
    if (!current_workspace_.enabled_commands.empty()) {
        return current_workspace_.enabled_commands.find(command_name) != current_workspace_.enabled_commands.end();
    }

    // По умолчанию все включено
    return true;
}

// =========================================================================
// Доступ к конфигурациям
// =========================================================================

std::vector<std::string> WorkspaceManager::getAvailableWorkspaces() const {
    std::vector<std::string> names;
    for (const auto& pair : workspaces_) {
        names.push_back(pair.first);
    }
    return names;
}

const WorkspaceConfiguration* WorkspaceManager::getWorkspaceConfig(const std::string& name) const {
    auto it = workspaces_.find(name);
    return (it != workspaces_.end()) ? &it->second : nullptr;
}

const WorkspaceConfiguration* WorkspaceManager::getWorkspaceConfig(WorkspaceType type) const {
    std::string name = workspace_type_to_string(type);
    return getWorkspaceConfig(name);
}

// =========================================================================
// Callbacks для уведомлений о переключении
// =========================================================================

void WorkspaceManager::addWorkspaceChangedCallback(WorkspaceChangedCallback callback) {
    if (callback) {
        workspace_changed_callbacks_.push_back(std::move(callback));
    }
}

void WorkspaceManager::removeWorkspaceChangedCallback(WorkspaceChangedCallback callback) {
    workspace_changed_callbacks_.erase(
        std::remove_if(workspace_changed_callbacks_.begin(), workspace_changed_callbacks_.end(),
            [&callback](const WorkspaceChangedCallback& cb) {
                return cb.target_type() == callback.target_type();
            }),
        workspace_changed_callbacks_.end()
    );
}

void WorkspaceManager::notifyWorkspaceChanged() {
    std::cout << "[WORKSPACE] notifyWorkspaceChanged: workspace = " << current_workspace_.name 
              << ", callbacks = " << workspace_changed_callbacks_.size() 
              << ", menu_callback = " << (menu_update_callback_ ? "SET" : "NULL") << std::endl;
    
    for (const auto& callback : workspace_changed_callbacks_) {
        if (callback) {
            callback(current_workspace_.type);
        }
    }

    // Вызываем callback для обновления меню
    if (menu_update_callback_) {
        std::cout << "[WORKSPACE] Calling menu_update_callback..." << std::endl;
        menu_update_callback_();
    }
}

void WorkspaceManager::setMenuUpdateCallback(MenuUpdateCallback callback) {
    menu_update_callback_ = std::move(callback);
}

// =========================================================================
// Сериализация
// =========================================================================

std::string WorkspaceManager::serializeToJson() const {
    json json_data;

    // Текущий workspace
    json_data["current_workspace"] = current_workspace_.name;

    // Все конфигурации
    json workspaces_json = json::object();
    for (const auto& pair : workspaces_) {
        const auto& config = pair.second;
        json workspace_json;
        workspace_json["name"] = config.name;
        workspace_json["type"] = workspace_type_to_string(config.type);
        workspace_json["show_developer_tools"] = config.show_developer_tools;
        workspace_json["show_admin_tools"] = config.show_admin_tools;

        // Меню конфигурации - сохраняем с menu_key для корректного поиска
        json menus_json = json::array();
        for (const auto& menu_config : config.menu_configs) {
            json menu_json;
            menu_json["menu_key"] = menu_config.menu_key;  // Постоянный ключ
            menu_json["menu_name"] = menu_config.menu_name;  // Переведённое название
            menu_json["visible"] = menu_config.visible;
            menu_json["visible_items"] = menu_config.visible_items;
            menu_json["hidden_items"] = menu_config.hidden_items;
            menus_json.push_back(menu_json);
        }
        workspace_json["menu_configs"] = menus_json;

        // Команды
        workspace_json["enabled_commands"] = std::vector<std::string>(
            config.enabled_commands.begin(), config.enabled_commands.end());
        workspace_json["disabled_commands"] = std::vector<std::string>(
            config.disabled_commands.begin(), config.disabled_commands.end());

        workspaces_json[pair.first] = workspace_json;
    }
    json_data["workspaces"] = workspaces_json;

    return json_data.dump(2);
}

bool WorkspaceManager::deserializeFromJson(const std::string& json_str) {
    try {
        json json_data = json::parse(json_str);

        // Сначала запоминаем имя текущего workspace
        std::string current_name;
        if (json_data.contains("current_workspace")) {
            current_name = json_data["current_workspace"];
        }

        // Загружаем конфигурации
        if (json_data.contains("workspaces") && json_data["workspaces"].is_object()) {
            for (const auto& [name, workspace_json] : json_data["workspaces"].items()) {
                WorkspaceConfiguration config;
                config.name = workspace_json.value("name", name);
                config.type = string_to_workspace_type(workspace_json.value("type", "User"));
                config.show_developer_tools = workspace_json.value("show_developer_tools", false);
                config.show_admin_tools = workspace_json.value("show_admin_tools", false);

                // Меню конфигурации
                if (workspace_json.contains("menu_configs") && workspace_json["menu_configs"].is_array()) {
                    for (const auto& menu_json : workspace_json["menu_configs"]) {
                        WorkspaceMenuConfig menu_config;
                        // Сначала пытаемся загрузить menu_key, если нет - используем menu_name как ключ
                        menu_config.menu_key = menu_json.value("menu_key", menu_json.value("menu_name", ""));
                        menu_config.menu_name = menu_json.value("menu_name", "");
                        menu_config.visible = menu_json.value("visible", true);
                        menu_config.visible_items = menu_json.value("visible_items", std::vector<std::string>{});
                        menu_config.hidden_items = menu_json.value("hidden_items", std::vector<std::string>{});
                        config.menu_configs.push_back(menu_config);
                    }
                }

                // Команды
                if (workspace_json.contains("enabled_commands") && workspace_json["enabled_commands"].is_array()) {
                    auto& cmds = workspace_json["enabled_commands"];
                    config.enabled_commands = std::unordered_set<std::string>(cmds.begin(), cmds.end());
                }
                if (workspace_json.contains("disabled_commands") && workspace_json["disabled_commands"].is_array()) {
                    auto& cmds = workspace_json["disabled_commands"];
                    config.disabled_commands = std::unordered_set<std::string>(cmds.begin(), cmds.end());
                }

                workspaces_[name] = config;
            }
        }

        // Теперь устанавливаем текущий workspace после загрузки всех конфигураций
        if (!current_name.empty()) {
            auto it = workspaces_.find(current_name);
            if (it != workspaces_.end()) {
                current_workspace_ = it->second;
                std::cout << "✓ WorkspaceManager: Current workspace set to '" << current_name << "'" << std::endl;
            } else {
                std::cerr << "⚠ WorkspaceManager: Workspace not found: " << current_name << std::endl;
            }
        }

        std::cout << "✓ WorkspaceManager: Loaded " << workspaces_.size() << " workspace configurations" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "WorkspaceManager: Failed to deserialize: " << e.what() << std::endl;
        return false;
    }
}

} // namespace ui
} // namespace llama_gui
