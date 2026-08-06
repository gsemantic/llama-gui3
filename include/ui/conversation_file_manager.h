#pragma once

#include <string>
#include <vector>

namespace llama_gui {
namespace core {
    class StateManager;
}

namespace ui {

// Forward declarations
class MainWindow;

using llama_gui::core::StateManager;

/**
 * ConversationFileManager - Управление файлами разговоров
 */
class ConversationFileManager {
public:
    explicit ConversationFileManager(StateManager& state_manager);
    ~ConversationFileManager() = default;

    // Открытие файлов
    void openFile();
    void openConversationFile();
    void openConversationFile(const std::string& file_path);
    
    // Сохранение
    void saveCurrentConversation();
    void saveCurrentConversationAs(const std::string& file_path);
    void exportConversations(const std::string& file_path);
    
    // Диалоги
    bool tryOpenConversationFileDialog(std::string& selected_path);
    bool trySaveConversationFileDialog(std::string& selected_path);
    
    // Состояние
    bool isConversationModified() const;
    void setConversationModified(bool modified);
    std::string getCurrentConversationPath() const;
    std::vector<std::string> getAllConversationPaths() const;
    
private:
    StateManager& state_manager_;
    std::string current_conversation_path_;
    bool conversation_modified_;
    
    // Внутренние методы
    bool trySaveConversationToPath(const std::string& file_path);
    void showSaveSuccess(const std::string& file_path);
    void showSaveError(const std::string& file_path);
};

} // namespace ui
} // namespace llama_gui
