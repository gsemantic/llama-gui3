#include "conversation_file_manager.h"
#include "main_window.h"
#include "file_dialog_helper.h"
#include <iostream>
#include <fstream>

namespace llama_gui {
namespace ui {

using llama_gui::core::StateManager;

ConversationFileManager::ConversationFileManager(StateManager& state_manager)
    : state_manager_(state_manager)
    , current_conversation_path_()
    , conversation_modified_(false) {
}

void ConversationFileManager::openFile() {
    std::cout << "ConversationFileManager: Opening file..." << std::endl;
    // TODO: Открыть диалог открытия файла
}

void ConversationFileManager::openConversationFile() {
    std::cout << "ConversationFileManager: Opening conversation file..." << std::endl;
    // TODO: Открыть диалог открытия conversation файла
}

void ConversationFileManager::openConversationFile(const std::string& file_path) {
    std::cout << "ConversationFileManager: Opening conversation from: " << file_path << std::endl;
    if (state_manager_.load_from_file(file_path)) {
        current_conversation_path_ = file_path;
        conversation_modified_ = false;
        std::cout << "ConversationFileManager: Successfully opened: " << file_path << std::endl;
    } else {
        std::cout << "ConversationFileManager: Failed to open: " << file_path << std::endl;
    }
}

void ConversationFileManager::saveCurrentConversation() {
    if (current_conversation_path_.empty()) {
        std::cout << "ConversationFileManager: No current conversation path, using save as..." << std::endl;
        // TODO: Сохранить как
        return;
    }

    if (trySaveConversationToPath(current_conversation_path_)) {
        conversation_modified_ = false;
        std::cout << "ConversationFileManager: Saved to: " << current_conversation_path_ << std::endl;
    } else {
        std::cout << "ConversationFileManager: Failed to save to: " << current_conversation_path_ << std::endl;
    }
}

void ConversationFileManager::saveCurrentConversationAs(const std::string& file_path) {
    std::string target_path = file_path;

    // If no extension, add .json
    if (target_path.find('.') == std::string::npos) {
        target_path += ".json";
    }

    if (trySaveConversationToPath(target_path)) {
        current_conversation_path_ = target_path;
        conversation_modified_ = false;
        std::cout << "ConversationFileManager: Saved as: " << current_conversation_path_ << std::endl;
    } else {
        std::cout << "ConversationFileManager: Failed to save as: " << target_path << std::endl;
    }
}

void ConversationFileManager::exportConversations(const std::string& file_path) {
    std::cout << "ConversationFileManager: Exporting conversations to: " << file_path << std::endl;
    // TODO: Реализовать экспорт всех разговоров
}

bool ConversationFileManager::tryOpenConversationFileDialog(std::string& selected_path) {
    std::cout << "ConversationFileManager: Opening conversation file dialog..." << std::endl;
    
#ifdef USE_SDL2
    FileDialogHelper helper;
    if (helper.open_file_dialog_sync("Open Conversation", selected_path, "json")) {
        if (!selected_path.empty()) {
            std::cout << "ConversationFileManager: Selected: " << selected_path << std::endl;
            return true;
        }
    }
#else
    std::cout << "ConversationFileManager: Native dialogs not supported on this platform" << std::endl;
#endif
    
    return false;
}

bool ConversationFileManager::trySaveConversationFileDialog(std::string& selected_path) {
    std::cout << "ConversationFileManager: Opening save conversation file dialog..." << std::endl;

#ifdef USE_SDL2
    FileDialogHelper helper;
    std::string default_name = "conversation.json";
    if (helper.save_file_dialog_sync("Save Conversation", default_name, selected_path, "json")) {
        if (!selected_path.empty()) {
            std::cout << "ConversationFileManager: Selected: " << selected_path << std::endl;
            return true;
        }
    }
#else
    std::cout << "ConversationFileManager: Native dialogs not supported on this platform" << std::endl;
#endif

    return false;
}

bool ConversationFileManager::isConversationModified() const {
    return conversation_modified_;
}

void ConversationFileManager::setConversationModified(bool modified) {
    conversation_modified_ = modified;
    std::cout << "ConversationFileManager: Modified flag set to: " << (modified ? "true" : "false") << std::endl;
}

std::string ConversationFileManager::getCurrentConversationPath() const {
    return current_conversation_path_;
}

std::vector<std::string> ConversationFileManager::getAllConversationPaths() const {
    std::vector<std::string> paths;
    // TODO: Получить все пути разговоров из state_manager_
    return paths;
}

bool ConversationFileManager::trySaveConversationToPath(const std::string& file_path) {
    std::cout << "ConversationFileManager: Saving conversation to: " << file_path << std::endl;
    
    if (state_manager_.save_to_file(file_path)) {
        std::cout << "ConversationFileManager: Successfully saved to: " << file_path << std::endl;
        return true;
    } else {
        std::cout << "ConversationFileManager: Failed to save to: " << file_path << std::endl;
        return false;
    }
}

} // namespace ui
} // namespace llama_gui
