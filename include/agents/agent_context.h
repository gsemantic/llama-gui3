#pragma once

#include "agent_types.h"
#include "agent_result.h"
#include "agent_request.h"
#include <string>
#include <memory>
#include <atomic>
#include <functional>
#include <nlohmann/json.hpp>

namespace agents {

class IAgent;
class AgentRegistry;

/**
 * @brief Контекст выполнения агента
 * 
 * Предоставляет агенту доступ к сервисам ядра и другим агентам.
 * Также управляет отменой операций и таймаутами.
 */
class AgentContext {
public:
    AgentContext();
    virtual ~AgentContext();

    /**
     * @brief Вызов другого агента
     * @param request Запрос к агенту
     * @return Результат выполнения
     * 
     * Позволяет агентам взаимодействовать друг с другом
     */
    AgentResult call_agent(const AgentRequest& request);

    /**
     * @brief Вызов другого агента по имени
     * @param agent_name Имя агента
     * @param action Действие
     * @param params Параметры
     * @return Результат выполнения
     */
    AgentResult call_agent(const std::string& agent_name, 
                           const std::string& action,
                           const nlohmann::json& params = {});

    /**
     * @brief Проверка отмены операции
     * @return true если операция отменена
     */
    bool is_cancelled() const;

    /**
     * @brief Запрос на отмену операции
     */
    void request_cancel();

    /**
     * @brief Проверка превышения таймаута
     * @return true если время вышло
     */
    bool is_timeout() const;

    /**
     * @brief Установка таймаута в миллисекундах
     */
    void set_timeout_ms(int timeout_ms);

    /**
     * @brief Логирование сообщения от имени агента
     */
    void log(const std::string& agent_name, LogLevel level, 
             const std::string& message);

    /**
     * @brief Логирование уровня DEBUG
     */
    void debug(const std::string& agent_name, const std::string& msg);

    /**
     * @brief Логирование уровня INFO
     */
    void info(const std::string& agent_name, const std::string& msg);

    /**
     * @brief Логирование уровня WARNING
     */
    void warning(const std::string& agent_name, const std::string& msg);

    /**
     * @brief Логирование уровня ERROR
     */
    void error(const std::string& agent_name, const std::string& msg);

    /**
     * @brief Получение значения из глобального хранилища
     */
    nlohmann::json get_state(const std::string& key) const;

    /**
     * @brief Установка значения в глобальное хранилище
     */
    void set_state(const std::string& key, const nlohmann::json& value);

    /**
     * @brief Проверка наличия ключа в хранилище
     */
    bool has_state(const std::string& key) const;

    /**
     * @brief Получение директории плагинов
     */
    std::string get_plugins_dir() const;

    /**
     * @brief Установка директории плагинов
     */
    void set_plugins_dir(const std::string& dir);

    /**
     * @brief Получение директории данных агентов
     */
    std::string get_data_dir() const;

    /**
     * @brief Установка директории данных
     */
    void set_data_dir(const std::string& dir);

    /**
     * @brief Получение конфигурации агента
     */
    nlohmann::json get_agent_config(const std::string& agent_name) const;

    /**
     * @brief Установка реестра агентов
     */
    void set_registry(AgentRegistry* registry);

    /**
     * @brief Получение реестра агентов
     */
    AgentRegistry* get_registry() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace agents
