#pragma once

/**
 * @file agent_commands.h
 * @brief Команды для вызова агентов через CommandManager
 */

#include <agents/agents.h>
#include <string>
#include <vector>
#include <functional>

namespace llama_gui {
namespace ui {

/**
 * @brief Результат выполнения команды агента
 */
struct AgentCommandResult {
    bool success;
    std::string message;
    std::string agent_name;
    std::string action;
    nlohmann::json data;
};

/**
 * @brief Менеджер команд агентов
 * 
 * Обрабатывает команды вида:
 * - /agent <name> <action> [params]
 * - /rag <query>
 * - /search <query>
 * - /summarize [text]
 * - /file <action> <path>
 * - /code <language> <prompt>
 * - /agents <command>
 */
class AgentCommands {
public:
    AgentCommands();
    ~AgentCommands();

    /**
     * @brief Инициализация менеджера команд
     * @param registry Реестр агентов
     * @param context Контекст агентов
     * @return true если успешно
     */
    bool initialize(agents::AgentRegistry* registry, agents::AgentContext* context);

    /**
     * @brief Обработка команды
     * @param command Полная строка команды
     * @return Результат выполнения
     */
    AgentCommandResult execute(const std::string& command);

    /**
     * @brief Обработка команды /agent
     */
    AgentCommandResult handle_agent_command(const std::vector<std::string>& args);

    /**
     * @brief Обработка команды /rag
     */
    AgentCommandResult handle_rag_command(const std::vector<std::string>& args);

    /**
     * @brief Обработка команды /search
     */
    AgentCommandResult handle_search_command(const std::vector<std::string>& args);

    /**
     * @brief Обработка команды /summarize
     */
    AgentCommandResult handle_summarize_command(const std::vector<std::string>& args);

    /**
     * @brief Обработка команды /file
     */
    AgentCommandResult handle_file_command(const std::vector<std::string>& args);

    /**
     * @brief Обработка команды /code
     */
    AgentCommandResult handle_code_command(const std::vector<std::string>& args);

    /**
     * @brief Обработка команды /agents
     */
    AgentCommandResult handle_agents_command(const std::vector<std::string>& args);

    /**
     * @brief Разбор строки команды на аргументы
     */
    static std::vector<std::string> parse_arguments(const std::string& command);

    /**
     * @brief Разбор параметров из строки
     */
    static nlohmann::json parse_params(const std::vector<std::string>& args);

    /**
     * @brief Установка колбэка на результат команды
     */
    void set_on_result(std::function<void(const AgentCommandResult&)> callback);

private:
    /**
     * @brief Форматирование результата для отображения
     */
    std::string format_result(const AgentCommandResult& result) const;

    /**
     * @brief Проверка доступности агента
     */
    bool is_agent_available(const std::string& name) const;

    agents::AgentRegistry* registry_ = nullptr;
    agents::AgentContext* context_ = nullptr;
    
    std::function<void(const AgentCommandResult&)> on_result_;
};

} // namespace ui
} // namespace llama_gui
