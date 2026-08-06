#pragma once

/**
 * @file agent_chat_integration.h
 * @brief Интеграция системы агентов с чат-интерфейсом
 */

#include "ui/agent_commands.h"
#include "ui/agent_panel.h"
#include "ui/agent_status_widget.h"
#include <agents/agents.h>
#include <string>
#include <functional>

namespace llama_gui {
namespace ui {

/**
 * @brief Результат выполнения команды для отображения в чате
 */
struct ChatAgentResult {
    bool success;
    std::string agent_name;
    std::string message;
    nlohmann::json data;
    int64_t timestamp;
};

/**
 * @brief Класс для интеграции агентов с чат-интерфейсом
 * 
 * Обрабатывает команды пользователя, выполняет их через агентов
 * и возвращает результаты для отображения в чате.
 */
class AgentChatIntegration {
public:
    AgentChatIntegration();
    ~AgentChatIntegration();

    /**
     * @brief Инициализация интеграции
     * @param registry Реестр агентов
     * @param context Контекст агентов
     * @return true если успешно
     */
    bool initialize(agents::AgentRegistry* registry, agents::AgentContext* context);

    /**
     * @brief Обработка команды из чата
     * @param command Текст команды
     * @param callback Колбэк для результата
     * @return true если команда обработана агентом
     */
    bool handle_chat_command(const std::string& command,
                              std::function<void(const ChatAgentResult&)> callback);

    /**
     * @brief Получение панели агентов
     */
    AgentPanel* get_agent_panel();

    /**
     * @brief Получение виджета статуса
     */
    AgentStatusWidget* get_status_widget();

    /**
     * @brief Получение менеджера команд
     */
    AgentCommands* get_agent_commands();

    /**
     * @brief Проверка доступности агентов
     */
    bool is_available() const;

    /**
     * @brief Получение количества активных агентов
     */
    int get_active_agent_count() const;

    /**
     * @brief Установка активного агента
     * @param name Имя агента
     */
    void set_active_agent(const std::string& name);

    /**
     * @brief Сброс активного агента
     */
    void clear_active_agent();

private:
    /**
     * @brief Форматирование результата для чата
     */
    std::string format_for_chat(const ChatAgentResult& result) const;

    /**
     * @brief Преобразование AgentCommandResult в ChatAgentResult
     */
    ChatAgentResult convert_result(const AgentCommandResult& cmd_result) const;

    agents::AgentRegistry* registry_ = nullptr;
    agents::AgentContext* context_ = nullptr;
    
    AgentPanel agent_panel_;
    AgentStatusWidget status_widget_;
    AgentCommands agent_commands_;
    
    std::string active_agent_;
    bool initialized_ = false;
};

} // namespace ui
} // namespace llama_gui
