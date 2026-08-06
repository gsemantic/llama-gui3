#pragma once

/**
 * @file terminal_agent.h
 * @brief Агент для выполнения команд терминала с песочницей
 */

#include <agents/agents.h>
#include <string>
#include <vector>
#include <unordered_set>
#include <memory>

namespace agents {

/**
 * @brief Агент для безопасного выполнения команд терминала
 * 
 * Поддерживаемые действия:
 * - exec - выполнение команды
 * - exec_safe - выполнение из белого списка
 * - list_commands - список разрешённых команд
 * - add_command - добавление в белый список
 * - remove_command - удаление из белого списка
 */
class TerminalAgent : public IAgent {
public:
    TerminalAgent();
    ~TerminalAgent() override;

    const char* name() const override;
    const char* description() const override;
    const char* version() const override;

    bool initialize(AgentContext* context) override;
    AgentResult execute(const AgentRequest& request) override;
    void shutdown() override;
    AgentCapability capabilities() const override;
    bool is_ready() const override;

private:
    // Обработчики действий
    AgentResult handle_exec(const AgentRequest& request);
    AgentResult handle_exec_safe(const AgentRequest& request);
    AgentResult handle_list_commands(const AgentRequest& request);
    AgentResult handle_add_command(const AgentRequest& request);
    AgentResult handle_remove_command(const AgentRequest& request);

    /**
     * @brief Проверка команды в белом списке
     */
    bool is_command_allowed(const std::string& command) const;

    /**
     * @brief Извлечение базовой команды
     */
    std::string extract_base_command(const std::string& command) const;

    /**
     * @brief Проверка на опасные команды
     */
    bool is_dangerous_command(const std::string& command) const;

    /**
     * @brief Выполнение команды через popen
     */
    AgentResult execute_command(const std::string& command, int timeout_ms);

    AgentContext* context_ = nullptr;
    bool initialized_ = false;

    // Настройки
    std::unordered_set<std::string> allowed_commands_;
    std::string default_shell_ = "/bin/bash";
    int default_timeout_ms_ = 10000;
    bool strict_mode_ = true;  // Только белый список
    
    // Статистика
    int execution_count_ = 0;
    int blocked_count_ = 0;
};

} // namespace agents
