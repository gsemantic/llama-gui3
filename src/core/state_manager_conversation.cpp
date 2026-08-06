#include "../include/core/state_manager_conversation.h"
#include <iostream>
#include <ctime>

namespace llama_gui {
namespace core {

StateManagerConversation::StateManagerConversation(StateManager& state_manager)
    : state_manager_(state_manager) {
}

std::string StateManagerConversation::create_conversation(const std::string& title) {
    auto& impl = *state_manager_.impl_;
    
    // Create a new conversation with the given title
    std::string conversation_id = "conversation_" + std::to_string(std::time(nullptr));

    // Create a new conversation object
    Conversation new_conversation(title);
    new_conversation.id = conversation_id;
    new_conversation.created_at = std::to_string(std::time(nullptr));
    new_conversation.updated_at = std::to_string(std::time(nullptr));
    new_conversation.is_active = true; // Set conversation as active

    // Add the conversation to our internal storage
    impl.conversations_[conversation_id] = new_conversation;

    // Set this as the active conversation
    impl.active_conversation_id_ = conversation_id;

    // Emit event about conversation creation
    if (state_manager_.conversation_callback_) {
        StateEvent event(StateEventType::ConversationCreated, conversation_id, "New conversation created: " + title);
        state_manager_.conversation_callback_(event);
    }

    return conversation_id;
}

bool StateManagerConversation::delete_conversation(const std::string& conversation_id) {
    auto& impl = *state_manager_.impl_;
    auto it = impl.conversations_.find(conversation_id);
    if (it != impl.conversations_.end()) {
        impl.conversations_.erase(it);
        return true;
    }
    return false;
}

bool StateManagerConversation::set_active_conversation(const std::string& conversation_id) {
    auto& impl = *state_manager_.impl_;
    auto it = impl.conversations_.find(conversation_id);
    if (it != impl.conversations_.end()) {
        // First, deactivate all conversations
        for (auto& pair : impl.conversations_) {
            pair.second.is_active = false;
        }

        // Then activate the selected conversation
        it->second.is_active = true;
        impl.active_conversation_id_ = conversation_id;
        return true;
    }
    return false;
}

Conversation* StateManagerConversation::get_conversation(const std::string& conversation_id) {
    auto& impl = *state_manager_.impl_;
    auto it = impl.conversations_.find(conversation_id);
    if (it != impl.conversations_.end()) {
        return &it->second;
    }
    return nullptr;
}

std::vector<Conversation*> StateManagerConversation::get_all_conversations() const {
    auto& impl = *state_manager_.impl_;
    std::vector<Conversation*> result;
    for (auto& pair : impl.conversations_) {
        result.push_back(&pair.second);
    }
    return result;
}

bool StateManagerConversation::add_message(const std::string& conversation_id, const Message& message) {
    auto& impl = *state_manager_.impl_;
    auto it = impl.conversations_.find(conversation_id);
    if (it != impl.conversations_.end()) {
        it->second.messages.push_back(message);
        it->second.updated_at = std::to_string(std::time(nullptr));

        // Notify UI about the change
        if (state_manager_.conversation_callback_) {
            StateEvent event(StateEventType::MessageAdded, conversation_id, "Message added to conversation");
            state_manager_.conversation_callback_(event);
        }

        return true;
    }
    return false;
}

bool StateManagerConversation::update_message(const std::string& conversation_id, const std::string& message_id, const std::string& content) {
    auto& impl = *state_manager_.impl_;
    auto it = impl.conversations_.find(conversation_id);
    if (it != impl.conversations_.end()) {
        for (auto& msg : it->second.messages) {
            if (msg.id == message_id) {
                msg.content = content;
                it->second.updated_at = std::to_string(std::time(nullptr));
                return true;
            }
        }
    }
    return false;
}

bool StateManagerConversation::delete_message(const std::string& conversation_id, const std::string& message_id) {
    auto& impl = *state_manager_.impl_;
    auto it = impl.conversations_.find(conversation_id);
    if (it != impl.conversations_.end()) {
        auto& messages = it->second.messages;
        for (auto msg_it = messages.begin(); msg_it != messages.end(); ++msg_it) {
            if (msg_it->id == message_id) {
                messages.erase(msg_it);
                it->second.updated_at = std::to_string(std::time(nullptr));
                return true;
            }
        }
    }
    return false;
}

std::vector<Message*> StateManagerConversation::get_messages(const std::string& conversation_id) const {
    auto& impl = *state_manager_.impl_;
    auto it = impl.conversations_.find(conversation_id);
    if (it != impl.conversations_.end()) {
        std::vector<Message*> result;
        for (auto& msg : it->second.messages) {
            result.push_back(&msg);
        }
        return result;
    }
    return {};
}

size_t StateManagerConversation::get_total_conversations() const {
    auto& impl = *state_manager_.impl_;
    return impl.conversations_.size();
}

size_t StateManagerConversation::get_total_messages() const {
    auto& impl = *state_manager_.impl_;
    size_t total = 0;
    for (const auto& pair : impl.conversations_) {
        total += pair.second.messages.size();
    }
    return total;
}

} // namespace core
} // namespace llama_gui
