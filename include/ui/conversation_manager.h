#pragma once

#include <string>
#include <vector>
#include <memory>
#include "core/llama_interface.h"
#include "core/state_manager.h"

namespace llama_gui {
namespace ui {

using StateManager = llama_gui::core::StateManager;
using Conversation = llama_gui::core::Conversation;

/**
 * Conversation manager component
 * Handles conversation sidebar, switching, and organization
 */
class ConversationManager {
public:
    ConversationManager(StateManager& state_manager);
    ~ConversationManager();

    // Main rendering
    void render(bool* visible = nullptr);

    // Conversation operations
    void create_new_conversation();
    void delete_conversation(const std::string& conversation_id);
    void rename_conversation(const std::string& conversation_id);
    void duplicate_conversation(const std::string& conversation_id);

    // Search and filtering
    void set_search_query(const std::string& query);
    void clear_search() { search_query_ = ""; }

    // Display options
    void set_show_timestamps(bool show) { show_timestamps_ = show; }
    void set_sort_by_name(bool sort_by_name) { sort_by_name_ = sort_by_name; }

private:
    void render_conversation_list();
    void render_conversation_item(const Conversation* conversation);
    void render_search_bar();
    void render_conversation_context_menu(const std::string& conversation_id);

    // Helpers
    std::string truncate_title(const std::string& title, size_t max_length = 30) const;
    std::string format_timestamp(long long timestamp) const;

    // State
    std::string search_query_;
    std::string selected_conversation_id_;
    bool show_timestamps_ = false;
    bool sort_by_name_ = true;
    bool show_context_menu_ = false;
    std::string context_menu_conversation_id_;

    // References
    StateManager& state_manager_;
};

} // namespace ui
} // namespace llama_gui
