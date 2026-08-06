#include "agents/agent_config.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>

namespace agents {

/**
 * @brief Внутренняя реализация AgentConfigManager
 */
class AgentConfigManager::Impl {
public:
    AgentsSystemConfig system_config;
    mutable std::mutex mutex;

    Impl() {
        // Конфигурация по умолчанию
        reset_to_defaults();
    }

    void reset_to_defaults() {
        system_config = AgentsSystemConfig();
        system_config.sandbox_config.timeout_ms = 30000;
        system_config.sandbox_config.max_memory_mb = 512;
        system_config.sandbox_config.max_output_size_kb = 1024;
    }
};

// ============================================================================

AgentConfigManager::AgentConfigManager() 
    : impl_(std::make_unique<Impl>()) {}

AgentConfigManager::~AgentConfigManager() = default;

bool AgentConfigManager::load_from_file(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "[AgentConfig] Cannot open file: " << filename << std::endl;
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string json_str = buffer.str();

    return load_from_json(json_str);
}

bool AgentConfigManager::save_to_file(const std::string& filename) {
    std::string json_str = save_to_json();

    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "[AgentConfig] Cannot create file: " << filename << std::endl;
        return false;
    }

    file << json_str;
    return true;
}

bool AgentConfigManager::load_from_json(const std::string& json_str) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    try {
        auto json = nlohmann::json::parse(json_str);

        // Основные настройки
        if (json.contains("plugins_directory")) {
            impl_->system_config.plugins_directory = 
                json["plugins_directory"].get<std::string>();
        }
        if (json.contains("user_plugins_directory")) {
            impl_->system_config.user_plugins_directory = 
                json["user_plugins_directory"].get<std::string>();
        }
        if (json.contains("official_plugins_directory")) {
            impl_->system_config.official_plugins_directory = 
                json["official_plugins_directory"].get<std::string>();
        }

        // Автозагрузка
        if (json.contains("auto_load_plugins")) {
            impl_->system_config.auto_load_plugins = 
                json["auto_load_plugins"].get<bool>();
        }
        if (json.contains("enable_hot_reload")) {
            impl_->system_config.enable_hot_reload = 
                json["enable_hot_reload"].get<bool>();
        }

        // Уровень безопасности
        if (json.contains("security_level")) {
            std::string level = json["security_level"].get<std::string>();
            if (level == "NONE") {
                impl_->system_config.security_level = SecurityLevel::NONE;
            } else if (level == "STANDARD") {
                impl_->system_config.security_level = SecurityLevel::STANDARD;
            } else if (level == "STRICT") {
                impl_->system_config.security_level = SecurityLevel::STRICT;
            } else if (level == "SANDBOX") {
                impl_->system_config.security_level = SecurityLevel::SANDBOX;
            }
        }

        // Включённые/отключённые агенты
        if (json.contains("enabled_agents")) {
            impl_->system_config.enabled_agents = 
                json["enabled_agents"].get<std::vector<std::string>>();
        }
        if (json.contains("disabled_agents")) {
            impl_->system_config.disabled_agents = 
                json["disabled_agents"].get<std::vector<std::string>>();
        }

        // Конфигурации агентов
        if (json.contains("agent_settings")) {
            for (auto& [name, config] : json["agent_settings"].items()) {
                AgentConfig agent_config;
                agent_config.name = name;

                if (config.contains("enabled")) {
                    agent_config.enabled = config["enabled"].get<bool>();
                }
                if (config.contains("timeout_ms")) {
                    agent_config.timeout_ms = config["timeout_ms"].get<int>();
                }
                if (config.contains("max_memory_mb")) {
                    agent_config.max_memory_mb = config["max_memory_mb"].get<int>();
                }
                if (config.contains("settings")) {
                    agent_config.settings = config["settings"];
                }

                impl_->system_config.agent_configs[name] = agent_config;
            }
        }

        // Логирование
        if (json.contains("enable_logging")) {
            impl_->system_config.enable_logging = 
                json["enable_logging"].get<bool>();
        }
        if (json.contains("log_level")) {
            std::string level = json["log_level"].get<std::string>();
            if (level == "DEBUG") {
                impl_->system_config.log_level = LogLevel::DEBUG;
            } else if (level == "INFO") {
                impl_->system_config.log_level = LogLevel::INFO;
            } else if (level == "WARNING") {
                impl_->system_config.log_level = LogLevel::WARNING;
            } else if (level == "ERROR") {
                impl_->system_config.log_level = LogLevel::ERROR;
            }
        }
        if (json.contains("log_file")) {
            impl_->system_config.log_file = 
                json["log_file"].get<std::string>();
        }

        // Песочница
        if (json.contains("enable_sandbox")) {
            impl_->system_config.enable_sandbox = 
                json["enable_sandbox"].get<bool>();
        }

        std::cout << "[AgentConfig] Loaded configuration from JSON" << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "[AgentConfig] Failed to parse JSON: " << e.what() << std::endl;
        return false;
    }
}

std::string AgentConfigManager::save_to_json() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    nlohmann::json json;

    // Директории
    json["plugins_directory"] = impl_->system_config.plugins_directory;
    json["user_plugins_directory"] = impl_->system_config.user_plugins_directory;
    json["official_plugins_directory"] = impl_->system_config.official_plugins_directory;

    // Автозагрузка
    json["auto_load_plugins"] = impl_->system_config.auto_load_plugins;
    json["enable_hot_reload"] = impl_->system_config.enable_hot_reload;

    // Уровень безопасности
    switch (impl_->system_config.security_level) {
        case SecurityLevel::NONE: json["security_level"] = "NONE"; break;
        case SecurityLevel::STANDARD: json["security_level"] = "STANDARD"; break;
        case SecurityLevel::STRICT: json["security_level"] = "STRICT"; break;
        case SecurityLevel::SANDBOX: json["security_level"] = "SANDBOX"; break;
    }

    // Агенты
    json["enabled_agents"] = impl_->system_config.enabled_agents;
    json["disabled_agents"] = impl_->system_config.disabled_agents;

    // Конфигурации агентов
    nlohmann::json agent_settings = nlohmann::json::object();
    for (const auto& [name, config] : impl_->system_config.agent_configs) {
        nlohmann::json agent_config;
        agent_config["enabled"] = config.enabled;
        agent_config["timeout_ms"] = config.timeout_ms;
        agent_config["max_memory_mb"] = config.max_memory_mb;
        agent_config["settings"] = config.settings;
        agent_settings[name] = agent_config;
    }
    json["agent_settings"] = agent_settings;

    // Логирование
    json["enable_logging"] = impl_->system_config.enable_logging;
    
    switch (impl_->system_config.log_level) {
        case LogLevel::DEBUG: json["log_level"] = "DEBUG"; break;
        case LogLevel::INFO: json["log_level"] = "INFO"; break;
        case LogLevel::WARNING: json["log_level"] = "WARNING"; break;
        case LogLevel::ERROR: json["log_level"] = "ERROR"; break;
        case LogLevel::CRITICAL: json["log_level"] = "CRITICAL"; break;
    }
    
    if (!impl_->system_config.log_file.empty()) {
        json["log_file"] = impl_->system_config.log_file;
    }

    // Песочница
    json["enable_sandbox"] = impl_->system_config.enable_sandbox;

    return json.dump(4);  // 4 пробела для форматирования
}

const AgentsSystemConfig& AgentConfigManager::get_system_config() const {
    return impl_->system_config;
}

void AgentConfigManager::set_system_config(const AgentsSystemConfig& config) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->system_config = config;
}

const AgentConfig* AgentConfigManager::get_agent_config(
        const std::string& agent_name) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    auto it = impl_->system_config.agent_configs.find(agent_name);
    if (it != impl_->system_config.agent_configs.end()) {
        return &it->second;
    }
    return nullptr;
}

void AgentConfigManager::set_agent_config(const std::string& agent_name,
                                           const AgentConfig& config) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->system_config.agent_configs[agent_name] = config;
}

bool AgentConfigManager::is_agent_enabled(const std::string& agent_name) const {
    auto config = get_agent_config(agent_name);
    if (config) {
        return config->enabled;
    }

    // Проверка списков включённых/отключённых
    const auto& sys_config = impl_->system_config;
    
    for (const auto& name : sys_config.disabled_agents) {
        if (name == agent_name) return false;
    }
    
    for (const auto& name : sys_config.enabled_agents) {
        if (name == agent_name) return true;
    }

    return true;  // По умолчанию включён
}

void AgentConfigManager::set_agent_enabled(const std::string& agent_name, 
                                            bool enabled) {
    auto config = get_agent_config(agent_name);
    if (config) {
        const_cast<AgentConfig*>(config)->enabled = enabled;
    } else {
        AgentConfig new_config;
        new_config.name = agent_name;
        new_config.enabled = enabled;
        set_agent_config(agent_name, new_config);
    }
}

void AgentConfigManager::set_agent_setting(const std::string& agent_name,
                                            const std::string& key,
                                            const nlohmann::json& value) {
    auto config = get_agent_config(agent_name);
    if (!config) {
        AgentConfig new_config;
        new_config.name = agent_name;
        new_config.settings[key] = value;
        set_agent_config(agent_name, new_config);
    } else {
        const_cast<AgentConfig*>(config)->settings[key] = value;
    }
}

std::vector<std::string> AgentConfigManager::get_plugin_directories() const {
    std::vector<std::string> dirs;
    
    const auto& config = impl_->system_config;
    
    if (!config.plugins_directory.empty()) {
        dirs.push_back(config.plugins_directory);
    }
    if (!config.user_plugins_directory.empty()) {
        dirs.push_back(config.user_plugins_directory);
    }
    if (!config.official_plugins_directory.empty()) {
        dirs.push_back(config.official_plugins_directory);
    }
    
    return dirs;
}

void AgentConfigManager::reset_to_defaults() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->reset_to_defaults();
}

bool AgentConfigManager::validate() const {
    const auto& config = impl_->system_config;

    // Проверка директорий
    if (!config.plugins_directory.empty() && 
        !std::filesystem::exists(config.plugins_directory)) {
        std::cerr << "[AgentConfig] Plugins directory does not exist: " 
                  << config.plugins_directory << std::endl;
        return false;
    }

    // Проверка timeout
    for (const auto& [name, agent_config] : config.agent_configs) {
        if (agent_config.timeout_ms <= 0) {
            std::cerr << "[AgentConfig] Invalid timeout for " << name << std::endl;
            return false;
        }
        if (agent_config.max_memory_mb <= 0) {
            std::cerr << "[AgentConfig] Invalid memory limit for " << name << std::endl;
            return false;
        }
    }

    return true;
}

} // namespace agents
