#pragma once

#include "agent_types.h"
#include "security_manager.h"
#include "sandbox.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>

namespace agents {

/**
 * @brief Конфигурация отдельного агента
 */
struct AgentConfig {
    std::string name;
    bool enabled = true;
    int timeout_ms = 30000;
    int max_memory_mb = 512;
    nlohmann::json settings;
    Permissions permissions;
};

/**
 * @brief Общая конфигурация системы агентов
 */
struct AgentsSystemConfig {
    std::string plugins_directory = "plugins";
    std::string user_plugins_directory = "plugins/user_plugins";
    std::string official_plugins_directory = "plugins/official";
    
    bool auto_load_plugins = true;
    bool enable_hot_reload = false;
    
    SecurityLevel security_level = SecurityLevel::STANDARD;
    
    std::vector<std::string> enabled_agents;
    std::vector<std::string> disabled_agents;
    
    std::unordered_map<std::string, AgentConfig> agent_configs;
    
    // Логирование
    bool enable_logging = true;
    LogLevel log_level = LogLevel::INFO;
    std::string log_file;
    
    // Песочница
    bool enable_sandbox = true;
    SandboxConfig sandbox_config;
};

/**
 * @brief Менеджер конфигурации агентов
 * 
 * Управляет загрузкой, сохранением и доступом к конфигурации.
 * Поддерживает JSON формат.
 */
class AgentConfigManager {
public:
    AgentConfigManager();
    ~AgentConfigManager();

    /**
     * @brief Загрузка конфигурации из файла
     * @param filename Имя файла
     * @return true если успешно
     */
    bool load_from_file(const std::string& filename);

    /**
     * @brief Сохранение конфигурации в файл
     * @param filename Имя файла
     * @return true если успешно
     */
    bool save_to_file(const std::string& filename);

    /**
     * @brief Загрузка из JSON строки
     * @param json_str JSON строка
     * @return true если успешно
     */
    bool load_from_json(const std::string& json_str);

    /**
     * @brief Сохранение в JSON строку
     * @return JSON строка
     */
    std::string save_to_json() const;

    /**
     * @brief Получение общей конфигурации
     */
    const AgentsSystemConfig& get_system_config() const;

    /**
     * @brief Установка общей конфигурации
     */
    void set_system_config(const AgentsSystemConfig& config);

    /**
     * @brief Получение конфигурации агента
     * @param agent_name Имя агента
     * @return Конфигурация или nullptr
     */
    const AgentConfig* get_agent_config(const std::string& agent_name) const;

    /**
     * @brief Установка конфигурации агента
     * @param agent_name Имя агента
     * @param config Конфигурация
     */
    void set_agent_config(const std::string& agent_name,
                           const AgentConfig& config);

    /**
     * @brief Проверка включённости агента
     */
    bool is_agent_enabled(const std::string& agent_name) const;

    /**
     * @brief Включение/отключение агента
     */
    void set_agent_enabled(const std::string& agent_name, bool enabled);

    /**
     * @brief Получение значения настройки агента
     */
    template<typename T>
    T get_agent_setting(const std::string& agent_name,
                        const std::string& key,
                        const T& default_value) const {
        auto config = get_agent_config(agent_name);
        if (!config) {
            return default_value;
        }
        
        if (config->settings.contains(key)) {
            return config->settings.value(key, default_value);
        }
        
        return default_value;
    }

    /**
     * @brief Установка значения настройки агента
     */
    void set_agent_setting(const std::string& agent_name,
                            const std::string& key,
                            const nlohmann::json& value);

    /**
     * @brief Список всех директорий плагинов
     */
    std::vector<std::string> get_plugin_directories() const;

    /**
     * @brief Сброс к конфигурации по умолчанию
     */
    void reset_to_defaults();

    /**
     * @brief Валидация конфигурации
     * @return true если конфигурация валидна
     */
    bool validate() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace agents
