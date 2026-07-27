#pragma once

/**
 * @file template_agent.h
 * @brief Шаблон для создания нового агента
 * 
 * Инструкция:
 * 1. Скопируйте этот файл в новую директорию агента
 * 2. Переименуйте TemplateAgent в YourAgent
 * 3. Измените name(), description(), version()
 * 4. Реализуйте свои действия в execute()
 * 5. Обновите plugin.json
 */

#include <agents/agents.h>
#include <string>

namespace agents {

class TemplateAgent : public IAgent {
public:
    TemplateAgent();
    ~TemplateAgent() override;

    /**
     * @brief Уникальное имя агента
     * Измените на своё название (например, "my_agent")
     */
    const char* name() const override;

    /**
     * @brief Описание агента
     * Измените на своё описание
     */
    const char* description() const override;

    /**
     * @brief Версия агента (semver)
     */
    const char* version() const override;

    /**
     * @brief Инициализация агента
     * Добавьте свою логику инициализации
     */
    bool initialize(AgentContext* context) override;

    /**
     * @brief Выполнение задачи агента
     * Добавьте свои действия
     */
    AgentResult execute(const AgentRequest& request) override;

    /**
     * @brief Остановка агента
     * Добавьте очистку ресурсов
     */
    void shutdown() override;

    /**
     * @brief Возможности агента
     * Верните нужные AgentCapability
     */
    AgentCapability capabilities() const override;

    /**
     * @brief Проверка готовности агента
     */
    bool is_ready() const override;

private:
    // ========================================================================
    // Обработчики действий
    // ========================================================================
    
    /**
     * @brief Обработчик действия по умолчанию
     * Добавьте свои обработчики здесь
     */
    AgentResult handle_default(const AgentRequest& request);
    
    // Примеры обработчиков:
    // AgentResult handle_action1(const AgentRequest& request);
    // AgentResult handle_action2(const AgentRequest& request);

    AgentContext* context_ = nullptr;
    bool initialized_ = false;
    
    // Настройки агента
    int timeout_ms_ = 30000;
};

} // namespace agents
