#pragma once

#include "agent_types.h"
#include <string>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace agents {

/**
 * @brief Запрос к агенту на выполнение задачи
 * 
 * Содержит имя агента, действие и параметры выполнения
 */
class AgentRequest {
public:
    AgentRequest() = default;

    AgentRequest(const std::string& agent_name, const std::string& action)
        : agent_name_(agent_name), action_(action) {}

    AgentRequest(const std::string& agent_name, const std::string& action,
                 const nlohmann::json& params)
        : agent_name_(agent_name), action_(action), params_(params) {}

    // Геттеры и сеттеры
    const std::string& agent_name() const { return agent_name_; }
    void set_agent_name(const std::string& name) { agent_name_ = name; }

    const std::string& action() const { return action_; }
    void set_action(const std::string& action) { action_ = action; }

    const nlohmann::json& params() const { return params_; }
    void set_params(const nlohmann::json& params) { params_ = params; }

    /**
     * @brief Получение параметра по имени
     */
    template<typename T>
    T get_param(const std::string& key, const T& default_value = T{}) const {
        if (params_.contains(key)) {
            return params_.value(key, default_value);
        }
        return default_value;
    }

    /**
     * @brief Проверка наличия параметра
     */
    bool has_param(const std::string& key) const {
        return params_.contains(key);
    }

    /**
     * @brief Добавление параметра
     */
    AgentRequest& with_param(const std::string& key, const nlohmann::json& value) {
        params_[key] = value;
        return *this;
    }

    /**
     * @brief Получение строкового параметра
     */
    std::string query() const {
        return get_param<std::string>("query", "");
    }

    /**
     * @brief Получение пути к файлу
     */
    std::string file_path() const {
        return get_param<std::string>("file_path", "");
    }

    /**
     * @brief Получение содержимого
     */
    std::string content() const {
        return get_param<std::string>("content", "");
    }

private:
    std::string agent_name_;     ///< Имя целевого агента
    std::string action_;         ///< Действие (например, "search", "read", "generate")
    nlohmann::json params_;      ///< Параметры выполнения
};

} // namespace agents
