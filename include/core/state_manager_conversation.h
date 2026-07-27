#pragma once

#include "state_manager.h"
#include <string>
#include <vector>

namespace llama_gui {
namespace core {

/**
 * @brief Управление беседами и сообщениями
 * 
 * Этот модуль содержит все операции с беседами и сообщениями:
 * - Создание/удаление бесед
 * - Активация беседы
 * - Добавление/обновление/удаление сообщений
 * - Получение списка бесед и сообщений
 */
class StateManagerConversation {
public:
    StateManagerConversation(StateManager& state_manager);
    ~StateManagerConversation() = default;

    // Запрет копирования
    StateManagerConversation(const StateManagerConversation&) = delete;
    StateManagerConversation& operator=(const StateManagerConversation&) = delete;

    // Управление диалогами
    std::string create_conversation(const std::string& title = "New Chat");
    bool delete_conversation(const std::string& conversation_id);
    bool set_active_conversation(const std::string& conversation_id);
    Conversation* get_conversation(const std::string& conversation_id);
    std::vector<Conversation*> get_all_conversations() const;

    // Управление сообщениями
    bool add_message(const std::string& conversation_id, const Message& message);
    bool update_message(const std::string& conversation_id, const std::string& message_id, const std::string& content);
    bool delete_message(const std::string& conversation_id, const std::string& message_id);
    std::vector<Message*> get_messages(const std::string& conversation_id) const;

    // Статистика
    size_t get_total_conversations() const;
    size_t get_total_messages() const;

private:
    StateManager& state_manager_;
};

} // namespace core
} // namespace llama_gui
