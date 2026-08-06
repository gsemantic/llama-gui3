#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <ctime>
#include <unordered_map>

namespace llama_gui {
namespace core {

/**
 * @brief Типы событий состояния
 */
enum class StateEventType {
    ConversationCreated,
    ConversationChanged,
    ConversationDeleted,
    MessageAdded,
    SettingsChanged,
    ServerConnected,
    ServerDisconnected,
    Error
};

/**
 * @brief Информация о событии состояния
 */
struct StateEvent {
    StateEventType type;
    std::string conversation_id;
    std::string message;
    std::string data;
    
    StateEvent(StateEventType t, const std::string& conv_id = "", const std::string& msg = "")
        : type(t), conversation_id(conv_id), message(msg) {}
};

/**
 * @brief Структура сообщения в диалоге
 */
struct Message {
    std::string id;
    std::string role;           // "user", "assistant", "system"
    std::string content;
    std::string timestamp;
    bool is_error = false;
    
    Message() = default;
    Message(const std::string& r, const std::string& c) 
        : role(r), content(c) {
        timestamp = std::to_string(std::time(nullptr));
    }
};

/**
 * @brief Структура диалога
 */
struct Conversation {
    std::string id;
    std::string title;
    std::vector<Message> messages;
    std::string created_at;
    std::string updated_at;
    bool is_active = false;
    
    Conversation() = default;
    explicit Conversation(const std::string& title) : title(title) {
        id = std::to_string(std::hash<std::string>{}(title));
        created_at = std::to_string(std::time(nullptr));
    }
};

// Forward declarations
class StateManagerConversation;
class StateManagerPersistence;

/**
 * @brief Менеджер состояния приложения
 * 
 * Делигирует операции соответствующим модулям:
 * - StateManagerConversation: управление беседами и сообщениями
 * - StateManagerPersistence: сохранение и загрузка состояния
 */
class StateManager {
public:
    using StateChangeCallback = std::function<void(const StateEvent&)>;

public:
    StateManager();
    ~StateManager();

    // Запрет копирования
    StateManager(const StateManager&) = delete;
    StateManager& operator=(const StateManager&) = delete;

    // Основные операции
    bool initialize(const class Settings& settings);
    void shutdown();

    // Управление диалогами (делегирование StateManagerConversation)
    std::string create_conversation(const std::string& title = "New Chat");
    bool delete_conversation(const std::string& conversation_id);
    bool set_active_conversation(const std::string& conversation_id);
    Conversation* get_conversation(const std::string& conversation_id);
    std::vector<Conversation*> get_all_conversations() const;

    // Управление сообщениями (делегирование StateManagerConversation)
    bool add_message(const std::string& conversation_id, const Message& message);
    bool update_message(const std::string& conversation_id, const std::string& message_id, const std::string& content);
    bool delete_message(const std::string& conversation_id, const std::string& message_id);
    std::vector<Message*> get_messages(const std::string& conversation_id) const;

    // Сериализация (делегирование StateManagerPersistence)
    bool save_to_file(const std::string& file_path);
    bool load_from_file(const std::string& file_path);
    std::string serialize_to_json() const;
    bool deserialize_from_json(const std::string& json_data);

    // Настройки обратных вызовов
    void set_conversation_change_callback(StateChangeCallback callback);
    void set_state_change_callback(StateChangeCallback callback);

    // Утилиты (делегирование соответствующим модулям)
    std::string generate_id() const;
    void clear_all_data();
    size_t get_total_conversations() const;
    size_t get_total_messages() const;

private:
    void emit_event(const StateEvent& event);
    void emit_state_change(const std::string& key, const std::string& value);

private:
    // Внутренняя реализация (доступна для модулей)
    class Impl {
    public:
        ~Impl() = default;
        std::unordered_map<std::string, Conversation> conversations_;
        std::string active_conversation_id_;
    };
    std::unique_ptr<Impl> impl_;
    
    StateChangeCallback conversation_callback_;
    StateChangeCallback state_callback_;
    class Settings* settings_ = nullptr;

    // Модули
    std::unique_ptr<StateManagerConversation> conversation_module_;
    std::unique_ptr<StateManagerPersistence> persistence_module_;

    // Друзья для доступа к impl_
    friend class StateManagerConversation;
    friend class StateManagerPersistence;
};

} // namespace core
} // namespace llama_gui
