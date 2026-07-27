#pragma once

#include "agent_types.h"
#include "agent_request.h"
#include "agent_result.h"
#include "agent_capabilities.h"

namespace agents {

class AgentContext;

/**
 * @brief Базовый интерфейс для всех агентов системы
 * 
 * Каждый агент должен реализовать этот интерфейс.
 * Агенты могут быть встроенными или загружаться как плагины.
 */
class IAgent {
public:
    virtual ~IAgent() = default;

    /**
     * @brief Уникальное имя агента
     * @return Имя агента (например, "rag_agent", "file_agent")
     */
    virtual const char* name() const = 0;

    /**
     * @brief Описание возможностей агента
     * @return Краткое описание на английском
     */
    virtual const char* description() const = 0;

    /**
     * @brief Версия агента в формате semver
     * @return Строка версии (например, "1.0.0")
     */
    virtual const char* version() const = 0;

    /**
     * @brief Инициализация агента
     * @param context Контекст выполнения
     * @return true если инициализация успешна
     */
    virtual bool initialize(AgentContext* context) = 0;

    /**
     * @brief Выполнение задачи агента
     * @param request Запрос на выполнение
     * @return Результат выполнения
     */
    virtual AgentResult execute(const AgentRequest& request) = 0;

    /**
     * @brief Остановка агента и очистка ресурсов
     */
    virtual void shutdown() = 0;

    /**
     * @brief Возможности агента
     * @return Битовая маска capabilities
     */
    virtual AgentCapability capabilities() const = 0;

    /**
     * @brief Проверка готовности агента
     * @return true если агент готов к работе
     */
    virtual bool is_ready() const { return true; }
};

} // namespace agents
