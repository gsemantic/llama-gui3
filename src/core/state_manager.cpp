#include "../include/core/state_manager.h"
#include "../include/core/state_manager_conversation.h"
#include "../include/core/state_manager_persistence.h"
#include "../include/core/settings.h"
#include <iostream>
#include <unordered_map>
#include <vector>
#include <memory>
#include <fstream>
#include <sstream>

namespace llama_gui {
namespace core {

StateManager::StateManager()
    : impl_(std::make_unique<Impl>())
    , conversation_module_(std::make_unique<StateManagerConversation>(*this))
    , persistence_module_(std::make_unique<StateManagerPersistence>(*this)) {
}

StateManager::~StateManager() = default;

bool StateManager::initialize(const Settings& settings) {
    (void)settings;
    std::cout << "StateManager initialized" << std::endl;
    return true;
}

void StateManager::shutdown() {
    // Cleanup
}

// Делегирование StateManagerConversation
std::string StateManager::create_conversation(const std::string& title) {
    return conversation_module_->create_conversation(title);
}

bool StateManager::delete_conversation(const std::string& conversation_id) {
    return conversation_module_->delete_conversation(conversation_id);
}

bool StateManager::set_active_conversation(const std::string& conversation_id) {
    return conversation_module_->set_active_conversation(conversation_id);
}

Conversation* StateManager::get_conversation(const std::string& conversation_id) {
    return conversation_module_->get_conversation(conversation_id);
}

std::vector<Conversation*> StateManager::get_all_conversations() const {
    return conversation_module_->get_all_conversations();
}

bool StateManager::add_message(const std::string& conversation_id, const Message& message) {
    return conversation_module_->add_message(conversation_id, message);
}

bool StateManager::update_message(const std::string& conversation_id, const std::string& message_id, const std::string& content) {
    return conversation_module_->update_message(conversation_id, message_id, content);
}

bool StateManager::delete_message(const std::string& conversation_id, const std::string& message_id) {
    return conversation_module_->delete_message(conversation_id, message_id);
}

std::vector<Message*> StateManager::get_messages(const std::string& conversation_id) const {
    return conversation_module_->get_messages(conversation_id);
}

size_t StateManager::get_total_conversations() const {
    return conversation_module_->get_total_conversations();
}

size_t StateManager::get_total_messages() const {
    return conversation_module_->get_total_messages();
}

// Делегирование StateManagerPersistence
bool StateManager::save_to_file(const std::string& file_path) {
    return persistence_module_->save_to_file(file_path);
}

bool StateManager::load_from_file(const std::string& file_path) {
    return persistence_module_->load_from_file(file_path);
}

std::string StateManager::serialize_to_json() const {
    return persistence_module_->serialize_to_json();
}

bool StateManager::deserialize_from_json(const std::string& json_data) {
    return persistence_module_->deserialize_from_json(json_data);
}

std::string StateManager::generate_id() const {
    return persistence_module_->generate_id();
}

void StateManager::clear_all_data() {
    persistence_module_->clear_all_data();
}

// Callbacks
void StateManager::set_conversation_change_callback(StateChangeCallback callback) {
    conversation_callback_ = std::move(callback);
}

void StateManager::set_state_change_callback(StateChangeCallback callback) {
    state_callback_ = std::move(callback);
}

void StateManager::emit_event(const StateEvent& event) {
    (void)event;
}

void StateManager::emit_state_change(const std::string& key, const std::string& value) {
    (void)key;
    (void)value;
}

} // namespace core
} // namespace llama_gui
