#include "../include/ui/conversation_manager.h"
#include "../external/imgui/imgui.h"
#include "../include/ui/localization_manager.h"
#include <iostream>

namespace llama_gui {
namespace ui {

ConversationManager::ConversationManager(StateManager& state_manager)
    : state_manager_(state_manager) {
}

ConversationManager::~ConversationManager() = default;

void ConversationManager::render(bool* visible) {
    // Стандартное окно ImGui с заголовком и кнопками управления
    // Кнопка закрытия (×) и сворачивания (─) рисуются автоматически ImGui
    if (!ImGui::Begin(TR("conversations.title"), visible, ImGuiWindowFlags_None)) {
        ImGui::End();
        return;
    }

    // Search bar
    render_search_bar();

    // New conversation button
    if (ImGui::Button(TR("conversations.new_chat"), ImVec2(-1, 0))) {
        create_new_conversation();
    }

    ImGui::Separator();

    // Conversation list
    render_conversation_list();

    ImGui::End();
}

void ConversationManager::create_new_conversation() {
    state_manager_.create_conversation("New Chat");
}

void ConversationManager::delete_conversation(const std::string& conversation_id) {
    state_manager_.delete_conversation(conversation_id);
}

void ConversationManager::rename_conversation(const std::string& conversation_id) {
    // Stub implementation - would open a rename dialog
    std::cout << "ConversationManager: Rename conversation " << conversation_id << std::endl;
}

void ConversationManager::duplicate_conversation(const std::string& conversation_id) {
    // Implement duplicate functionality using available methods
    const auto* original_conv = state_manager_.get_conversation(conversation_id);
    if (original_conv) {
        std::string new_title = original_conv->title + " (Copy)";
        std::string new_conv_id = state_manager_.create_conversation(new_title);

        // Copy messages from original to new conversation
        const auto* new_conv = state_manager_.get_conversation(new_conv_id);
        if (new_conv) {
            for (const auto& msg : original_conv->messages) {
                state_manager_.add_message(new_conv_id, msg);
            }
        }
    }
}

void ConversationManager::set_search_query(const std::string& query) {
    search_query_ = query;
}

void ConversationManager::render_conversation_list() {
    ImGui::BeginChild("ConversationList", ImVec2(0, 0), true);

    auto conversations = state_manager_.get_all_conversations();

    for (const auto* conv : conversations) {
        if (conv) {
            render_conversation_item(conv);
        }
    }

    ImGui::EndChild();
}

void ConversationManager::render_conversation_item(const Conversation* conversation) {
    if (!conversation) return;

    // Push unique ID to avoid conflicts
    ImGui::PushID(conversation->id.c_str());

    // Check if active conversation
    auto all_conversations = state_manager_.get_all_conversations();
    const llama_gui::core::Conversation* active_conv = nullptr;
    for (const auto* conv : all_conversations) {
        if (conv->is_active) {
            active_conv = conv;
            break;
        }
    }
    bool is_active = (active_conv && active_conv->id == conversation->id);

    // Background color for active conversation
    if (is_active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.8f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.5f, 0.9f, 1.0f));
    }

    // Conversation button
    std::string button_text = truncate_title(conversation->title, 25);
    if (ImGui::Button(button_text.c_str(), ImVec2(-1, 0))) {
        state_manager_.set_active_conversation(conversation->id);
    }

    if (is_active) {
        ImGui::PopStyleColor(2);
    }

    // Context menu (right-click on button)
    if (ImGui::BeginPopupContextItem("ConversationContextMenu")) {
        if (ImGui::MenuItem(TR("conversations.rename"))) {
            rename_conversation(conversation->id);
        }
        if (ImGui::MenuItem(TR("conversations.duplicate"))) {
            duplicate_conversation(conversation->id);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(TR("conversations.delete"))) {
            delete_conversation(conversation->id);
        }
        ImGui::EndPopup();
    }

    // Pop unique ID
    ImGui::PopID();
}

void ConversationManager::render_search_bar() {
    char buffer[256];
    strcpy(buffer, search_query_.c_str());

    if (ImGui::InputText(TR("conversations.search"), buffer, sizeof(buffer))) {
        set_search_query(buffer);
    }
}

void ConversationManager::render_conversation_context_menu(const std::string& conversation_id) {
    // Stub implementation
}

std::string ConversationManager::truncate_title(const std::string& title, size_t max_length) const {
    if (title.length() <= max_length) return title;
    return title.substr(0, max_length - 3) + "...";
}

std::string ConversationManager::format_timestamp(long long timestamp) const {
    // Stub implementation
    return "Just now";
}

} // namespace ui
} // namespace llama_gui
