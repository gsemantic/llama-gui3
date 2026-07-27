/**
 * @file agent_chat_integration.cpp
 * @brief Реализация интеграции агентов с чат-интерфейсом
 */

#include "ui/agent_chat_integration.h"
#include <chrono>
#include <sstream>

namespace llama_gui {
namespace ui {

// ============================================================================
// AgentChatIntegration implementation
// ============================================================================

AgentChatIntegration::AgentChatIntegration() = default;

AgentChatIntegration::~AgentChatIntegration() = default;

bool AgentChatIntegration::initialize(agents::AgentRegistry* registry,
                                       agents::AgentContext* context) {
    if (!registry || !context) {
        return false;
    }
    
    registry_ = registry;
    context_ = context;
    
    // Инициализация компонентов
    if (!agent_panel_.initialize(registry_)) {
        return false;
    }
    
    status_widget_.initialize(registry_);
    
    if (!agent_commands_.initialize(registry_, context_)) {
        return false;
    }
    
    // Настройка колбэков
    agent_panel_.set_on_agent_selected([this](const std::string& name) {
        set_active_agent(name);
    });
    
    agent_panel_.set_on_agent_action([this](const std::string& name,
                                             const std::string& action) {
        if (action == "enable" || action == "disable") {
            status_widget_.update();
        }
    });
    
    initialized_ = true;
    status_widget_.update();
    
    return true;
}

bool AgentChatIntegration::handle_chat_command(
        const std::string& command,
        std::function<void(const ChatAgentResult&)> callback) {
    
    if (!initialized_) {
        return false;
    }
    
    // Проверка на команду агента
    if (command.empty() || command[0] != '/') {
        return false;
    }
    
    // Обработка команды
    auto cmd_result = agent_commands_.execute(command);
    auto chat_result = convert_result(cmd_result);
    
    // Обновление статуса
    status_widget_.update();
    
    // Вызов колбэка
    if (callback) {
        callback(chat_result);
    }
    
    return true;
}

AgentPanel* AgentChatIntegration::get_agent_panel() {
    return &agent_panel_;
}

AgentStatusWidget* AgentChatIntegration::get_status_widget() {
    return &status_widget_;
}

AgentCommands* AgentChatIntegration::get_agent_commands() {
    return &agent_commands_;
}

bool AgentChatIntegration::is_available() const {
    return initialized_ && registry_ != nullptr;
}

int AgentChatIntegration::get_active_agent_count() const {
    return status_widget_.get_active_count();
}

void AgentChatIntegration::set_active_agent(const std::string& name) {
    active_agent_ = name;
    status_widget_.set_active_agent(name);
}

void AgentChatIntegration::clear_active_agent() {
    active_agent_.clear();
    status_widget_.clear_active_agent();
}

// ============================================================================
// Вспомогательные функции
// ============================================================================

ChatAgentResult AgentChatIntegration::convert_result(
        const AgentCommandResult& cmd_result) const {
    
    ChatAgentResult result;
    result.success = cmd_result.success;
    result.agent_name = cmd_result.agent_name;
    result.message = cmd_result.message;
    result.data = cmd_result.data;
    result.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    
    return result;
}

std::string AgentChatIntegration::format_for_chat(
        const ChatAgentResult& result) const {
    
    std::ostringstream oss;
    
    // Иконка статуса
    oss << (result.success ? "✅ " : "❌ ");
    
    // Имя агента
    oss << "[" << result.agent_name << "] ";
    
    // Сообщение
    oss << result.message;
    
    // Данные (если есть)
    if (!result.data.empty() && result.data.is_object()) {
        oss << "\n```\n";
        oss << result.data.dump(2);
        oss << "\n```";
    }
    
    return oss.str();
}

} // namespace ui
} // namespace llama_gui
