#include "../include/core/state_manager_persistence.h"
#include <iostream>
#include <fstream>
#include <sstream>

namespace llama_gui {
namespace core {

StateManagerPersistence::StateManagerPersistence(StateManager& state_manager)
    : state_manager_(state_manager) {
}

bool StateManagerPersistence::save_to_file(const std::string& file_path) {
    std::string json = serialize_to_json();
    if (json.empty()) {
        std::cerr << "StateManager: Failed to serialize state to JSON" << std::endl;
        return false;
    }

    std::ofstream file(file_path);
    if (!file.is_open()) {
        std::cerr << "StateManager: Failed to open file for writing: " << file_path << std::endl;
        return false;
    }

    file << json;
    file.close();

    std::cout << "StateManager: Saved state to " << file_path << std::endl;
    return true;
}

bool StateManagerPersistence::load_from_file(const std::string& file_path) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        std::cerr << "StateManager: Failed to open file for reading: " << file_path << std::endl;
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string json_data = buffer.str();
    file.close();

    if (!deserialize_from_json(json_data)) {
        std::cerr << "StateManager: Failed to deserialize JSON from file" << std::endl;
        return false;
    }

    std::cout << "StateManager: Loaded state from " << file_path << std::endl;
    return true;
}

std::string StateManagerPersistence::serialize_to_json() const {
    auto& impl = *state_manager_.impl_;
    std::ostringstream json;

    json << "{\n";
    json << "  \"version\": 1,\n";
    json << "  \"active_conversation_id\": \"" << impl.active_conversation_id_ << "\",\n";
    json << "  \"conversations\": [\n";

    bool first_conv = true;
    for (const auto& conv_pair : impl.conversations_) {
        const Conversation& conv = conv_pair.second;

        if (!first_conv) {
            json << ",\n";
        }
        first_conv = false;

        json << "    {\n";
        json << "      \"id\": \"" << conv.id << "\",\n";
        json << "      \"title\": \"" << conv.title << "\",\n";
        json << "      \"created_at\": \"" << conv.created_at << "\",\n";
        json << "      \"updated_at\": \"" << conv.updated_at << "\",\n";
        json << "      \"is_active\": " << (conv.is_active ? "true" : "false") << ",\n";
        json << "      \"messages\": [\n";

        bool first_msg = true;
        for (const auto& msg : conv.messages) {
            if (!first_msg) {
                json << ",\n";
            }
            first_msg = false;

            json << "        {\n";
            json << "          \"id\": \"" << msg.id << "\",\n";
            json << "          \"role\": \"" << msg.role << "\",\n";
            json << "          \"content\": \"";

            // Escape special characters in content
            for (char c : msg.content) {
                switch (c) {
                    case '"': json << "\\\""; break;
                    case '\\': json << "\\\\"; break;
                    case '\n': json << "\\n"; break;
                    case '\r': json << "\\r"; break;
                    case '\t': json << "\\t"; break;
                    default: json << c; break;
                }
            }

            json << "\",\n";
            json << "          \"timestamp\": \"" << msg.timestamp << "\",\n";
            json << "          \"is_error\": " << (msg.is_error ? "true" : "false") << "\n";
            json << "        }";
        }

        json << "\n      ]\n";
        json << "    }";
    }

    json << "\n  ]\n";
    json << "}\n";

    return json.str();
}

bool StateManagerPersistence::deserialize_from_json(const std::string& json_data) {
    if (json_data.empty()) {
        std::cerr << "StateManager: Empty JSON data" << std::endl;
        return false;
    }

    // Helper lambda to find unescaped quote (not preceded by backslash)
    auto find_unescaped_quote = [](const std::string& str, size_t start) -> size_t {
        size_t pos = str.find('"', start);
        while (pos != std::string::npos) {
            // Count backslashes before this quote
            int backslash_count = 0;
            size_t check_pos = pos;
            while (check_pos > 0 && str[check_pos - 1] == '\\') {
                backslash_count++;
                check_pos--;
            }
            // If even number of backslashes (or zero), quote is not escaped
            if (backslash_count % 2 == 0) {
                return pos;
            }
            // Otherwise, find next quote
            pos = str.find('"', pos + 1);
        }
        return std::string::npos;
    };

    // Simple JSON parsing without external library
    // Parse conversations array
    size_t conversations_start = json_data.find("\"conversations\"");
    if (conversations_start == std::string::npos) {
        std::cerr << "StateManager: No conversations found in JSON" << std::endl;
        return false;
    }

    // Find the opening bracket of the conversations array
    size_t array_start = json_data.find('[', conversations_start);
    if (array_start == std::string::npos) {
        std::cerr << "StateManager: Invalid JSON format" << std::endl;
        return false;
    }

    // Find matching closing bracket, accounting for content inside strings
    int bracket_count = 1;
    bool in_string = false;
    size_t array_end = array_start + 1;
    while (bracket_count > 0 && array_end < json_data.size()) {
        char c = json_data[array_end];
        // Check for unescaped quote
        bool is_unescaped_quote = (c == '"');
        if (is_unescaped_quote) {
            int backslash_count = 0;
            size_t check_pos = array_end;
            while (check_pos > 0 && json_data[check_pos - 1] == '\\') {
                backslash_count++;
                check_pos--;
            }
            is_unescaped_quote = (backslash_count % 2 == 0);
        }
        
        if (c == '"' && is_unescaped_quote) {
            in_string = !in_string;
        } else if (!in_string) {
            if (c == '[') bracket_count++;
            else if (c == ']') bracket_count--;
        }
        array_end++;
    }

    if (bracket_count != 0) {
        std::cerr << "StateManager: Unbalanced brackets in JSON" << std::endl;
        return false;
    }

    std::string conversations_json = json_data.substr(array_start, array_end - array_start);

    // Clear existing data
    auto& impl = *state_manager_.impl_;
    impl.conversations_.clear();
    impl.active_conversation_id_.clear();

    // Parse each conversation object
    size_t pos = 0;
    while ((pos = conversations_json.find("{", pos)) != std::string::npos) {
        // Find matching closing brace, accounting for nested objects and strings
        int brace_count = 1;
        bool in_string = false;
        size_t obj_end = pos + 1;
        while (obj_end < conversations_json.size() && brace_count > 0) {
            char c = conversations_json[obj_end];
            // Check for unescaped quote
            bool is_unescaped_quote = (c == '"');
            if (is_unescaped_quote) {
                int backslash_count = 0;
                size_t check_pos = obj_end;
                while (check_pos > 0 && conversations_json[check_pos - 1] == '\\') {
                    backslash_count++;
                    check_pos--;
                }
                is_unescaped_quote = (backslash_count % 2 == 0);
            }
            
            if (c == '"' && is_unescaped_quote) {
                in_string = !in_string;
            } else if (!in_string) {
                if (c == '{') brace_count++;
                else if (c == '}') brace_count--;
            }
            obj_end++;
        }
        if (brace_count != 0) break;

        std::string conv_json = conversations_json.substr(pos, obj_end - pos);

        // Parse conversation fields
        Conversation conv;

        // Parse id
        size_t id_start = conv_json.find("\"id\"");
        if (id_start != std::string::npos) {
            size_t id_val_start = find_unescaped_quote(conv_json, id_start + 4);
            size_t id_val_end = find_unescaped_quote(conv_json, id_val_start + 1);
            if (id_val_start != std::string::npos && id_val_end != std::string::npos) {
                conv.id = conv_json.substr(id_val_start + 1, id_val_end - id_val_start - 1);
            }
        }

        // Parse title
        size_t title_start = conv_json.find("\"title\"");
        if (title_start != std::string::npos) {
            size_t title_val_start = find_unescaped_quote(conv_json, title_start + 7);
            size_t title_val_end = find_unescaped_quote(conv_json, title_val_start + 1);
            if (title_val_start != std::string::npos && title_val_end != std::string::npos) {
                conv.title = conv_json.substr(title_val_start + 1, title_val_end - title_val_start - 1);
            }
        }

        // Parse created_at
        size_t created_start = conv_json.find("\"created_at\"");
        if (created_start != std::string::npos) {
            size_t created_val_start = find_unescaped_quote(conv_json, created_start + 11);
            size_t created_val_end = find_unescaped_quote(conv_json, created_val_start + 1);
            if (created_val_start != std::string::npos && created_val_end != std::string::npos) {
                conv.created_at = conv_json.substr(created_val_start + 1, created_val_end - created_val_start - 1);
            }
        }

        // Parse updated_at
        size_t updated_start = conv_json.find("\"updated_at\"");
        if (updated_start != std::string::npos) {
            size_t updated_val_start = find_unescaped_quote(conv_json, updated_start + 11);
            size_t updated_val_end = find_unescaped_quote(conv_json, updated_val_start + 1);
            if (updated_val_start != std::string::npos && updated_val_end != std::string::npos) {
                conv.updated_at = conv_json.substr(updated_val_start + 1, updated_val_end - updated_val_start - 1);
            }
        }

        // Parse is_active
        size_t active_start = conv_json.find("\"is_active\"");
        if (active_start != std::string::npos) {
            size_t colon_pos = conv_json.find(":", active_start);
            if (colon_pos != std::string::npos) {
                size_t value_start = colon_pos + 1;
                while (value_start < conv_json.size() && 
                       (conv_json[value_start] == ' ' || conv_json[value_start] == '\t')) {
                    value_start++;
                }
                
                if (conv_json.compare(value_start, 4, "true") == 0) {
                    if (impl.active_conversation_id_.empty()) {
                        conv.is_active = true;
                        impl.active_conversation_id_ = conv.id;
                    }
                } else {
                    conv.is_active = false;
                }
            }
        }

        // Parse messages
        size_t messages_start = conv_json.find("\"messages\"");
        if (messages_start != std::string::npos) {
            size_t msg_array_start = conv_json.find('[', messages_start);
            if (msg_array_start != std::string::npos) {
                size_t msg_array_end = 0;
                size_t pos = msg_array_start + 1;
                int bracket_count = 1;
                bool in_string = false;
                while (pos < conv_json.size() && bracket_count > 0) {
                    char c = conv_json[pos];
                    bool is_unescaped_quote = (c == '"');
                    if (is_unescaped_quote) {
                        int backslash_count = 0;
                        size_t check_pos = pos;
                        while (check_pos > 0 && conv_json[check_pos - 1] == '\\') {
                            backslash_count++;
                            check_pos--;
                        }
                        is_unescaped_quote = (backslash_count % 2 == 0);
                    }
                    
                    if (c == '"' && is_unescaped_quote) {
                        in_string = !in_string;
                    } else if (!in_string) {
                        if (c == '[') {
                            bracket_count++;
                        }
                        else if (c == ']') {
                            bracket_count--;
                        }
                    }
                    
                    if (bracket_count == 0) {
                        msg_array_end = pos + 1;
                        break;
                    }
                    pos++;
                }

                if (msg_array_start != std::string::npos && msg_array_end <= conv_json.size()) {
                    size_t msg_array_length = msg_array_end - msg_array_start - 1;
                    std::string messages_json = conv_json.substr(msg_array_start + 1, msg_array_length);

                    size_t msg_pos = 0;
                    while ((msg_pos = messages_json.find("{", msg_pos)) != std::string::npos) {
                        size_t msg_obj_end = messages_json.find("}", msg_pos);
                        if (msg_obj_end == std::string::npos) break;

                        std::string msg_json = messages_json.substr(msg_pos, msg_obj_end - msg_pos + 1);
                        Message msg;

                        // Parse role
                        size_t role_start = msg_json.find("\"role\"");
                        if (role_start != std::string::npos) {
                            size_t role_val_start = find_unescaped_quote(msg_json, role_start + 6);
                            size_t role_val_end = find_unescaped_quote(msg_json, role_val_start + 1);
                            if (role_val_start != std::string::npos && role_val_end != std::string::npos) {
                                msg.role = msg_json.substr(role_val_start + 1, role_val_end - role_val_start - 1);
                            }
                        }

                        // Parse content
                        size_t content_start = msg_json.find("\"content\"");
                        if (content_start != std::string::npos) {
                            size_t content_val_start = find_unescaped_quote(msg_json, content_start + 9);
                            size_t content_val_end = find_unescaped_quote(msg_json, content_val_start + 1);
                            if (content_val_start != std::string::npos && content_val_end != std::string::npos) {
                                msg.content = msg_json.substr(content_val_start + 1, content_val_end - content_val_start - 1);
                                // Unescape content
                                size_t escape_pos = 0;
                                while ((escape_pos = msg.content.find("\\", escape_pos)) != std::string::npos) {
                                    if (escape_pos + 1 < msg.content.size()) {
                                        char next = msg.content[escape_pos + 1];
                                        if (next == '"') {
                                            msg.content.replace(escape_pos, 2, "\"");
                                        } else if (next == 'n') {
                                            msg.content.replace(escape_pos, 2, "\n");
                                        } else if (next == 'r') {
                                            msg.content.replace(escape_pos, 2, "\r");
                                        } else if (next == 't') {
                                            msg.content.replace(escape_pos, 2, "\t");
                                        } else if (next == '\\') {
                                            msg.content.replace(escape_pos, 2, "\\");
                                        } else {
                                            escape_pos++;
                                        }
                                    } else {
                                        escape_pos++;
                                    }
                                }
                            }
                        }

                        // Parse id
                        size_t msg_id_start = msg_json.find("\"id\"");
                        if (msg_id_start != std::string::npos) {
                            size_t msg_id_val_start = find_unescaped_quote(msg_json, msg_id_start + 4);
                            size_t msg_id_val_end = find_unescaped_quote(msg_json, msg_id_val_start + 1);
                            if (msg_id_val_start != std::string::npos && msg_id_val_end != std::string::npos) {
                                msg.id = msg_json.substr(msg_id_val_start + 1, msg_id_val_end - msg_id_val_start - 1);
                            }
                        }

                        // Parse timestamp
                        size_t ts_start = msg_json.find("\"timestamp\"");
                        if (ts_start != std::string::npos) {
                            size_t ts_val_start = find_unescaped_quote(msg_json, ts_start + 11);
                            size_t ts_val_end = find_unescaped_quote(msg_json, ts_val_start + 1);
                            if (ts_val_start != std::string::npos && ts_val_end != std::string::npos) {
                                msg.timestamp = msg_json.substr(ts_val_start + 1, ts_val_end - ts_val_start - 1);
                            }
                        }

                        // Parse is_error
                        size_t err_start = msg_json.find("\"is_error\"");
                        if (err_start != std::string::npos) {
                            size_t err_true = msg_json.find("true", err_start);
                            size_t err_false = msg_json.find("false", err_start);
                            if (err_true != std::string::npos &&
                                (err_false == std::string::npos || err_true < err_false)) {
                                msg.is_error = true;
                            }
                        }

                        // Add message even if id is empty (generate one if needed)
                        if (msg.id.empty()) {
                            msg.id = generate_id();
                        }
                        conv.messages.push_back(msg);

                        msg_pos = msg_obj_end + 1;
                    }
                }
            }
        }

        if (!conv.id.empty()) {
            impl.conversations_[conv.id] = std::move(conv);
        }

        pos = obj_end + 1;
    }

    std::cout << "StateManager: Loaded " << impl.conversations_.size() << " conversations" << std::endl;
    
    // Debug output: show all loaded conversations
    for (const auto& pair : impl.conversations_) {
        std::cout << "  - Conversation: id=" << pair.second.id 
                  << ", title=" << pair.second.title
                  << ", is_active=" << (pair.second.is_active ? "true" : "false")
                  << ", messages=" << pair.second.messages.size() << std::endl;
    }
    
    // Notify UI about the change (conversations were loaded/reset)
    if (state_manager_.conversation_callback_ && !impl.active_conversation_id_.empty()) {
        StateEvent event(StateEventType::ConversationChanged, impl.active_conversation_id_, "Conversations loaded from file");
        state_manager_.conversation_callback_(event);
    }
    
    return true;
}

std::string StateManagerPersistence::generate_id() const {
    return "id_" + std::to_string(std::time(nullptr));
}

void StateManagerPersistence::clear_all_data() {
    auto& impl = *state_manager_.impl_;
    impl.conversations_.clear();
    impl.active_conversation_id_.clear();
}

} // namespace core
} // namespace llama_gui
