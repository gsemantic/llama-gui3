#pragma once

#include "agent_types.h"
#include <string>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace agents {

/**
 * @brief Результат выполнения агента
 * 
 * Содержит статус выполнения, данные и сообщение об ошибке
 */
class AgentResult {
public:
    AgentResult() : status_(AgentStatus::OK) {}

    explicit AgentResult(AgentStatus status) : status_(status) {}

    AgentResult(AgentStatus status, const std::string& message)
        : status_(status), message_(message) {}

    /**
     * @brief Создание успешного результата с данными
     */
    static AgentResult success(const nlohmann::json& data) {
        AgentResult result(AgentStatus::OK);
        result.data_ = data;
        return result;
    }

    /**
     * @brief Создание результата с ошибкой
     */
    static AgentResult error(const std::string& message) {
        return AgentResult(AgentStatus::ERROR, message);
    }

    /**
     * @brief Создание результата "не найдено"
     */
    static AgentResult not_found(const std::string& message) {
        return AgentResult(AgentStatus::NOT_FOUND, message);
    }

    /**
     * @brief Создание результата "отменено"
     */
    static AgentResult cancelled(const std::string& message) {
        return AgentResult(AgentStatus::CANCELLED, message);
    }

    // Геттеры
    AgentStatus status() const { return status_; }
    bool is_ok() const { return status_ == AgentStatus::OK; }
    
    const std::string& message() const { return message_; }
    void set_message(const std::string& msg) { message_ = msg; }
    
    const nlohmann::json& data() const { return data_; }
    void set_data(const nlohmann::json& data) { data_ = data; }

    /**
     * @brief Получение поля из данных
     */
    template<typename T>
    T get(const std::string& key, const T& default_value = T{}) const {
        if (data_.contains(key)) {
            return data_.value(key, default_value);
        }
        return default_value;
    }

    /**
     * @brief Добавление поля в данные
     */
    AgentResult& with(const std::string& key, const nlohmann::json& value) {
        data_[key] = value;
        return *this;
    }

private:
    AgentStatus status_;
    std::string message_;
    nlohmann::json data_;
};

} // namespace agents
