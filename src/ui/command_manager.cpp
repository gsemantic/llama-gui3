#include "../include/ui/command_manager.h"
#include "../include/ui/command.h"
#include "../include/ui/localization_manager.h"
#include <algorithm>
#include <iostream>

namespace llama_gui {
namespace ui {

// CommandManager implementation

bool CommandManager::registerCommand(const std::string& name, std::unique_ptr<Command> command) {
    if (!command || name.empty()) {
        return false;
    }
    
    // Проверяем, что команда с таким именем еще не зарегистрирована
    if (commands_.find(name) != commands_.end()) {
        std::cerr << TRF("general.error", "Error") << ": Command '" << name << "' is already registered" << std::endl;
        return false;
    }
    
    commands_[name] = std::move(command);
    std::cout << "✓ Registered command: " << name << std::endl;
    return true;
}

bool CommandManager::registerShortcut(const std::string& shortcut, const std::string& command_name) {
    if (shortcut.empty() || command_name.empty()) {
        return false;
    }
    
    // Проверяем, что команда существует
    if (commands_.find(command_name) == commands_.end()) {
        std::cerr << TRF("general.error", "Error") << ": Cannot register shortcut: command '" << command_name << "' not found" << std::endl;
        return false;
    }
    
    // Проверяем, что горячая клавиша еще не используется
    if (shortcuts_.find(shortcut) != shortcuts_.end()) {
        std::cerr << TRF("general.error", "Error") << ": Shortcut '" << shortcut << "' is already registered for command '" 
                  << shortcuts_[shortcut] << "'" << std::endl;
        return false;
    }
    
    shortcuts_[shortcut] = command_name;
    std::cout << "✓ Registered shortcut: " << shortcut << " -> " << command_name << std::endl;
    return true;
}

CommandManager::CommandResult CommandManager::executeCommand(const std::string& name) {
    // Интеграция с агентами - обработка команд агентов - ОТКЛЮЧЕНО
    // if (agent_chat_integration_ && !name.empty() && name[0] == '/') {
    //     // Это команда агента
    //     bool handled = agent_chat_integration_->handle_chat_command(name, nullptr);
    //     if (handled) {
    //         return CommandResult{true, "Agent command executed", ""};
    //     }
    // }

    return executeCommandInternal(name);
}

CommandManager::CommandResult CommandManager::executeShortcut(const std::string& shortcut) {
    auto it = shortcuts_.find(shortcut);
    if (it == shortcuts_.end()) {
        return CommandResult{false, "", "Shortcut '" + shortcut + "' not found"};
    }
    
    return executeCommandInternal(it->second);
}

bool CommandManager::undo() {
    if (command_history_.empty()) {
        return false;
    }
    
    std::string command_name = command_history_.top();
    command_history_.pop();
    
    auto it = commands_.find(command_name);
    if (it != commands_.end() && it->second) {
        it->second->undo();
        
        if (on_undo_) {
            on_undo_(command_name);
        }
        
        std::cout << "✓ Undone command: " << command_name << std::endl;
        return true;
    }
    
    return false;
}

void CommandManager::clearHistory() {
    while (!command_history_.empty()) {
        command_history_.pop();
    }
    std::cout << "✓ Cleared command history" << std::endl;
}

bool CommandManager::canUndo() const {
    return !command_history_.empty();
}

Command* CommandManager::getCommand(const std::string& name) {
    auto it = commands_.find(name);
    return (it != commands_.end()) ? it->second.get() : nullptr;
}

std::vector<std::string> CommandManager::getAllCommandNames() const {
    std::vector<std::string> names;
    names.reserve(commands_.size());
    
    for (const auto& pair : commands_) {
        names.push_back(pair.first);
    }
    
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> CommandManager::getAvailableShortcuts() const {
    std::vector<std::string> shortcuts;
    shortcuts.reserve(shortcuts_.size());
    
    for (const auto& pair : shortcuts_) {
        shortcuts.push_back(pair.first);
    }
    
    std::sort(shortcuts.begin(), shortcuts.end());
    return shortcuts;
}

std::string CommandManager::getCommandForShortcut(const std::string& shortcut) const {
    auto it = shortcuts_.find(shortcut);
    return (it != shortcuts_.end()) ? it->second : "";
}

bool CommandManager::isCommandRegistered(const std::string& name) const {
    return commands_.find(name) != commands_.end();
}

bool CommandManager::isShortcutRegistered(const std::string& shortcut) const {
    return shortcuts_.find(shortcut) != shortcuts_.end();
}

void CommandManager::setCommandExecutedCallback(std::function<void(const std::string&)> callback) {
    on_command_executed_ = std::move(callback);
}

void CommandManager::setCommandFailedCallback(std::function<void(const std::string&)> callback) {
    on_command_failed_ = std::move(callback);
}

void CommandManager::setUndoCallback(std::function<void(const std::string&)> callback) {
    on_undo_ = std::move(callback);
}

bool CommandManager::unregisterCommand(const std::string& name) {
    auto it = commands_.find(name);
    if (it == commands_.end()) {
        return false;
    }
    
    // Удаляем все горячие клавиши, связанные с этой командой
    std::vector<std::string> shortcuts_to_remove;
    for (const auto& shortcut_pair : shortcuts_) {
        if (shortcut_pair.second == name) {
            shortcuts_to_remove.push_back(shortcut_pair.first);
        }
    }
    
    for (const auto& shortcut : shortcuts_to_remove) {
        shortcuts_.erase(shortcut);
    }
    
    commands_.erase(it);
    std::cout << "✓ Unregistered command: " << name << std::endl;
    return true;
}

bool CommandManager::unregisterShortcut(const std::string& shortcut) {
    auto it = shortcuts_.find(shortcut);
    if (it == shortcuts_.end()) {
        return false;
    }
    
    shortcuts_.erase(it);
    std::cout << "✓ Unregistered shortcut: " << shortcut << std::endl;
    return true;
}

void CommandManager::initializeDefaultCommands(
    std::function<void()> new_chat_callback,
    std::function<void()> exit_callback,
    std::function<void()> settings_callback,
    std::function<void()> model_callback,
    std::function<void(ServerControlCommand::Action)> server_callback,
    std::function<void(const std::string&)> help_callback) {

    std::cout << "Initializing default commands..." << std::endl;

    // Регистрируем базовые команды
    registerCommand("exit_app", CommandFactory::createExitCommand(exit_callback));
    registerCommand("open_settings", CommandFactory::createOpenSettingsCommand(settings_callback));
    registerCommand("select_model", CommandFactory::createSelectModelCommand(model_callback));
    registerCommand("server_start", CommandFactory::createServerControlCommand(
        ServerControlCommand::Action::Start, server_callback));
    registerCommand("server_stop", CommandFactory::createServerControlCommand(
        ServerControlCommand::Action::Stop, server_callback));
    registerCommand("server_restart", CommandFactory::createServerControlCommand(
        ServerControlCommand::Action::Restart, server_callback));
    registerCommand("server_status", CommandFactory::createServerControlCommand(
        ServerControlCommand::Action::Status, server_callback));
    registerCommand("show_help", CommandFactory::createShowHelpCommand(help_callback));
    registerCommand("show_about", CommandFactory::createShowHelpCommand(help_callback, "about"));
    registerCommand("show_shortcuts", CommandFactory::createShowHelpCommand(help_callback, "shortcuts"));

    // =========================================================================
    // Дополнительные команды
    // =========================================================================

    // Register additional commands with callbacks (to be filled by MainWindow)
    registerCommand("export_conversation_json", CommandFactory::createFunctionalCommand(
        "export_conversation_json",
        []() { std::cout << "Export conversation JSON not implemented yet" << std::endl; },
        "Export conversation as JSON",
        "",
        nullptr));

    registerCommand("show_metrics", CommandFactory::createFunctionalCommand(
        "show_metrics",
        []() { 
            std::cout << "Showing Dear ImGui Metrics/Debugger" << std::endl;
            // Show ImGui Metrics Debugger
            std::cout << "ImGui Metrics Debugger: Please check ImGui::ShowMetricsWindow() implementation" << std::endl;
        },
        "Show Dear ImGui Metrics/Debugger",
        "",
        nullptr));
    registerCommand("open_file", CommandFactory::createFunctionalCommand(
        "open_file",
        [this]() {
            if (open_file_callback_) {
                open_file_callback_();
            } else {
                std::cout << "Open file callback not set" << std::endl;
            }
        },
        "Open a conversation file",
        "",
        nullptr));
    registerCommand("save_file", CommandFactory::createFunctionalCommand(
        "save_file",
        [this]() {
            if (save_file_callback_) {
                save_file_callback_();
            } else {
                std::cout << "Save file callback not set" << std::endl;
            }
        },
        "Save current conversation",
        "",
        nullptr));
    registerCommand("save_file_as", CommandFactory::createFunctionalCommand(
        "save_file_as",
        [this]() {
            if (save_file_as_callback_) {
                save_file_as_callback_("");
            } else {
                std::cout << "Save file as callback not set" << std::endl;
            }
        },
        "Save conversation as...",
        "",
        nullptr));

    // =========================================================================
    // Команды переключения окон
    // =========================================================================

    registerCommand("toggle_window_conversations", CommandFactory::createFunctionalCommand(
        "toggle_window_conversations",
        [this]() {
            if (toggle_window_callback_) {
                toggle_window_callback_("conversations");
            } else {
                std::cout << "Toggle window callback not set" << std::endl;
            }
        },
        "Toggle Conversations Panel",
        "",
        nullptr));

    registerCommand("toggle_window_chat", CommandFactory::createFunctionalCommand(
        "toggle_window_chat",
        [this]() {
            if (toggle_window_callback_) {
                toggle_window_callback_("chat");
            } else {
                std::cout << "Toggle window callback not set" << std::endl;
            }
        },
        "Toggle Chat Panel",
        "",
        nullptr));

    registerCommand("toggle_window_files", CommandFactory::createFunctionalCommand(
        "toggle_window_files",
        [this]() {
            if (toggle_window_callback_) {
                toggle_window_callback_("files");
            } else {
                std::cout << "Toggle window callback not set" << std::endl;
            }
        },
        "Toggle Files Panel",
        "",
        nullptr));

    registerCommand("toggle_window_rag", CommandFactory::createFunctionalCommand(
        "toggle_window_rag",
        [this]() {
            if (toggle_window_callback_) {
                toggle_window_callback_("rag");
            } else {
                std::cout << "Toggle window callback not set" << std::endl;
            }
        },
        "Toggle RAG Panel",
        "",
        nullptr));

    registerCommand("toggle_window_agents", CommandFactory::createFunctionalCommand(
        "toggle_window_agents",
        [this]() {
            if (toggle_window_callback_) {
                toggle_window_callback_("agents");
            } else {
                std::cout << "Toggle window callback not set" << std::endl;
            }
        },
        "Toggle Agents Panel",
        "",
        nullptr));

    registerCommand("toggle_window_settings", CommandFactory::createFunctionalCommand(
        "toggle_window_settings",
        [this]() {
            if (toggle_window_callback_) {
                toggle_window_callback_("settings");
            } else {
                std::cout << "Toggle window callback not set" << std::endl;
            }
        },
        "Toggle Settings Dialog",
        "",
        nullptr));

    registerCommand("export_settings", CommandFactory::createFunctionalCommand(
        "export_settings",
        []() { std::cout << "Export settings not implemented yet" << std::endl; },
        "Export application settings",
        "",
        nullptr));
    registerCommand("import_settings", CommandFactory::createFunctionalCommand(
        "import_settings",
        []() { std::cout << "Import settings not implemented yet" << std::endl; },
        "Import application settings",
        "",
        nullptr));
    registerCommand("export_chat_history", CommandFactory::createFunctionalCommand(
        "export_chat_history",
        []() { std::cout << "Export chat history not implemented yet" << std::endl; },
        "Export chat history",
        "",
        nullptr));
    registerCommand("import_conversation", CommandFactory::createFunctionalCommand(
        "import_conversation",
        []() { std::cout << "Import conversation not implemented yet" << std::endl; },
        "Import conversation",
        "",
        nullptr));
    // Commands for additional dialog windows
    registerCommand("toggle_window_cloud_services", CommandFactory::createFunctionalCommand(
        "toggle_window_cloud_services",
        [this]() {
            if (toggle_window_callback_) {
                toggle_window_callback_("cloud_services");
            } else {
                std::cout << "Toggle window callback not set" << std::endl;
            }
        },
        "Toggle Cloud Services Dialog",
        "",
        nullptr));

    registerCommand("toggle_window_rag_settings", CommandFactory::createFunctionalCommand(
        "toggle_window_rag_settings",
        [this]() {
            if (toggle_window_callback_) {
                toggle_window_callback_("rag_settings");
            } else {
                std::cout << "Toggle window callback not set" << std::endl;
            }
        },
        "Toggle RAG Settings Dialog",
        "",
        nullptr));

    registerCommand("toggle_window_settings_viewer", CommandFactory::createFunctionalCommand(
        "toggle_window_settings_viewer",
        [this]() {
            if (toggle_window_callback_) {
                toggle_window_callback_("settings_viewer");
            } else {
                std::cout << "Toggle window callback not set" << std::endl;
            }
        },
        "Toggle Settings Viewer Dialog",
        "",
        nullptr));

    registerCommand("toggle_window_profile_manager", CommandFactory::createFunctionalCommand(
        "toggle_window_profile_manager",
        [this]() {
            if (toggle_window_callback_) {
                toggle_window_callback_("profile_manager");
            } else {
                std::cout << "Toggle window callback not set" << std::endl;
            }
        },
        "Toggle Profile Manager Dialog",
        "",
        nullptr));

    registerCommand("toggle_window_backup_manager", CommandFactory::createFunctionalCommand(
        "toggle_window_backup_manager",
        [this]() {
            if (toggle_window_callback_) {
                toggle_window_callback_("backup_manager");
            } else {
                std::cout << "Toggle window callback not set" << std::endl;
            }
        },
        "Toggle Backup Manager Dialog",
        "",
        nullptr));

    // Toggle status bar command (имя "toggle_window_status_bar" совпадает с createWindowToggleItem)
    registerCommand("toggle_window_status_bar", CommandFactory::createFunctionalCommand(
        "toggle_window_status_bar",
        [this]() {
            if (toggle_window_callback_) {
                toggle_window_callback_("status_bar");
            } else {
                std::cout << "Toggle window callback not set" << std::endl;
            }
        },
        "Toggle Status Bar",
        "",
        nullptr));

    // Workspace commands are registered in MainWindow::initializeCommandCallbacks() with proper callbacks
    // registerCommand("save_workspace", ...);  // Moved to main_window_commands.cpp
    // registerCommand("load_workspace", ...);  // Moved to main_window_commands.cpp
    // registerCommand("reset_workspace", ...); // Moved to main_window_commands.cpp

    // Theme commands
    registerCommand("set_theme_dark", CommandFactory::createFunctionalCommand(
        "set_theme_dark", 
        []() { std::cout << "Set theme: Dark" << std::endl; },
        "Set dark theme",
        "",
        nullptr));
    registerCommand("set_theme_light", CommandFactory::createFunctionalCommand(
        "set_theme_light", 
        []() { std::cout << "Set theme: Light" << std::endl; },
        "Set light theme",
        "",
        nullptr));
    registerCommand("set_theme_classic", CommandFactory::createFunctionalCommand(
        "set_theme_classic", 
        []() { std::cout << "Set theme: Classic" << std::endl; },
        "Set classic theme",
        "",
        nullptr));
    
    // Регистрируем горячие клавиши
    registerShortcut("Ctrl+O", "open_file");
    registerShortcut("Ctrl+S", "save_file");
    registerShortcut("Alt+F4", "exit_app");
    registerShortcut("Ctrl+,", "open_settings");
    
    // Регистрируем горячие клавиши для переключения окон
    registerShortcut("Ctrl+1", "toggle_window_conversations");
    registerShortcut("Ctrl+2", "toggle_window_files");
    registerShortcut("Ctrl+3", "toggle_window_chat");
    registerShortcut("Ctrl+4", "toggle_window_rag");
    registerShortcut("Ctrl+5", "toggle_window_agents");
    
    std::cout << "✓ Default commands initialized (" << commands_.size() << " commands, " 
              << shortcuts_.size() << " shortcuts)" << std::endl;
}

CommandManager::Statistics CommandManager::getStatistics() const {
    Statistics stats;
    stats.total_commands = commands_.size();
    stats.total_shortcuts = shortcuts_.size();
    stats.history_size = command_history_.size();
    return stats;
}

void CommandManager::addToHistory(const std::string& command_name) {
    command_history_.push(command_name);
}

CommandManager::CommandResult CommandManager::executeCommandInternal(const std::string& name) {
    CommandResult result;
    
    auto it = commands_.find(name);
    if (it == commands_.end()) {
        result.error = "Command '" + name + "' not found";
        std::cerr << "❌ " << result.error << std::endl;
        
        if (on_command_failed_) {
            on_command_failed_(name);
        }
        
        return result;
    }
    
    if (!it->second->canExecute()) {
        result.error = "Command '" + name + "' cannot be executed";
        std::cerr << "⚠️  " << result.error << std::endl;
        
        if (on_command_failed_) {
            on_command_failed_(name);
        }
        
        return result;
    }

    std::cout << "[CommandManager] Command canExecute() = true, about to execute..." << std::endl;

    try {
        // Выполняем команду
        it->second->execute();
        
        // Добавляем в историю для возможной отмены
        addToHistory(name);
        
        result.success = true;
        result.message = "Command '" + name + "' executed successfully";
        
        std::cout << "✓ " << TRF("general.success", "Success") << ": " << result.message << std::endl;
        
        if (on_command_executed_) {
            on_command_executed_(name);
        }
        
    } catch (const std::exception& e) {
        result.error = "Exception while executing command '" + name + "': " + e.what();
        std::cerr << "❌ " << result.error << std::endl;
        
        if (on_command_failed_) {
            on_command_failed_(name);
        }
    } catch (...) {
        result.error = "Unknown exception while executing command '" + name + "'";
        std::cerr << "❌ " << result.error << std::endl;
        
        if (on_command_failed_) {
            on_command_failed_(name);
        }
    }
    
    return result;
}

// CommandFactory implementation

namespace CommandFactory {

std::unique_ptr<NewChatCommand> createNewChatCommand(std::function<void()> callback) {
    return std::make_unique<NewChatCommand>(std::move(callback));
}

std::unique_ptr<ExitCommand> createExitCommand(std::function<void()> callback) {
    return std::make_unique<ExitCommand>(std::move(callback));
}

std::unique_ptr<OpenSettingsCommand> createOpenSettingsCommand(std::function<void()> callback) {
    return std::make_unique<OpenSettingsCommand>(std::move(callback));
}

std::unique_ptr<SelectModelCommand> createSelectModelCommand(std::function<void()> callback) {
    return std::make_unique<SelectModelCommand>(std::move(callback));
}

std::unique_ptr<ServerControlCommand> createServerControlCommand(
    ServerControlCommand::Action action,
    std::function<void(ServerControlCommand::Action)> callback) {
    return std::make_unique<ServerControlCommand>(action, std::move(callback));
}

std::unique_ptr<ShowHelpCommand> createShowHelpCommand(
    std::function<void(const std::string&)> callback,
    const std::string& help_type) {
    return std::make_unique<ShowHelpCommand>(std::move(callback), help_type);
}

std::unique_ptr<FunctionalCommand> createFunctionalCommand(
    const std::string& name,
    std::function<void()> execute_func,
    const std::string& description,
    const std::string& shortcut,
    std::function<bool()> can_execute_func) {
    return std::make_unique<FunctionalCommand>(
        name, std::move(execute_func), description, shortcut, std::move(can_execute_func));
}

// Additional factory functions for new commands
std::unique_ptr<ExportConversationJsonCommand> createExportConversationJsonCommand(std::function<void()> callback) {
    return std::make_unique<ExportConversationJsonCommand>(std::move(callback));
}

std::unique_ptr<SetThemeCommand> createSetThemeCommand(const std::string& theme_name, std::function<void(const std::string&)> callback) {
    return std::make_unique<SetThemeCommand>(theme_name, std::move(callback));
}

std::unique_ptr<OpenFileCommand> createOpenFileCommand(std::function<void()> callback) {
    return std::make_unique<OpenFileCommand>(std::move(callback));
}

std::unique_ptr<SaveFileCommand> createSaveFileCommand(std::function<void()> callback) {
    return std::make_unique<SaveFileCommand>(std::move(callback));
}

std::unique_ptr<SaveFileAsCommand> createSaveFileAsCommand(std::function<void()> callback) {
    return std::make_unique<SaveFileAsCommand>(std::move(callback));
}

std::unique_ptr<WorkspaceCommand> createWorkspaceCommand(
    WorkspaceCommand::Action action,
    std::function<void(WorkspaceCommand::Action, const std::string&)> callback,
    const std::string& workspace_name) {
    return std::make_unique<WorkspaceCommand>(action, std::move(callback), workspace_name);
}

} // namespace CommandFactory

} // namespace ui
} // namespace llama_gui