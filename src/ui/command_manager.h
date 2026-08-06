#pragma once

#include "command.h"
#include <unordered_map>
#include <vector>
#include <memory>
#include <stack>
#include <string>
#include <functional>

namespace llama_gui {
namespace ui {

// Менеджер команд для централизованного управления всеми операциями
class CommandManager {
public:
    // Результат выполнения команды
    struct CommandResult {
        bool success = false;
        std::string message;
        std::string error;
    };
    
private:
    // Зарегистрированные команды
    std::unordered_map<std::string, std::unique_ptr<Command>> commands_;
    
    // Словарь горячих клавиш -> имя команды
    std::unordered_map<std::string, std::string> shortcuts_;
    
    // История выполненных команд для отмены
    std::stack<std::string> command_history_;
    
    // Функции обратного вызова для различных событий
    std::function<void(const std::string&)> on_command_executed_;
    std::function<void(const std::string&)> on_command_failed_;
    std::function<void(const std::string&)> on_undo_;
    
public:
    CommandManager() = default;
    ~CommandManager() = default;
    
    // Удалить копирование и перемещение
    CommandManager(const CommandManager&) = delete;
    CommandManager& operator=(const CommandManager&) = delete;
    CommandManager(CommandManager&&) = delete;
    CommandManager& operator=(CommandManager&&) = delete;
    
    // Регистрация команд
    bool registerCommand(const std::string& name, std::unique_ptr<Command> command);
    bool registerShortcut(const std::string& shortcut, const std::string& command_name);
    
    // Выполнение команд
    CommandResult executeCommand(const std::string& name);
    CommandResult executeShortcut(const std::string& shortcut);
    
    // Управление историей
    bool undo();
    void clearHistory();
    bool canUndo() const;
    
    // Получение информации о командах
    Command* getCommand(const std::string& name);
    std::vector<std::string> getAllCommandNames() const;
    std::vector<std::string> getAvailableShortcuts() const;
    std::string getCommandForShortcut(const std::string& shortcut) const;
    
    // Проверка доступности
    bool isCommandRegistered(const std::string& name) const;
    bool isShortcutRegistered(const std::string& shortcut) const;
    
    // Установка обработчиков событий
    void setCommandExecutedCallback(std::function<void(const std::string&)> callback);
    void setCommandFailedCallback(std::function<void(const std::string&)> callback);
    void setUndoCallback(std::function<void(const std::string&)> callback);
    
    // Удаление команд и горячих клавиш
    bool unregisterCommand(const std::string& name);
    bool unregisterShortcut(const std::string& shortcut);
    
    // Инициализация стандартных команд
    void initializeDefaultCommands(std::function<void()> new_chat_callback,
                                   std::function<void()> exit_callback,
                                   std::function<void()> settings_callback,
                                   std::function<void()> model_callback,
                                   std::function<void(ServerControlCommand::Action)> server_callback,
                                   std::function<void(const std::string&)> help_callback);
    
    // Получение статистики
    struct Statistics {
        size_t total_commands = 0;
        size_t total_shortcuts = 0;
        size_t history_size = 0;
    };
    
    Statistics getStatistics() const;
    
private:
    // Вспомогательные методы
    void addToHistory(const std::string& command_name);
    CommandResult executeCommandInternal(const std::string& name);
};

// Вспомогательные функции для создания команд
namespace CommandFactory {
    
    // Создать команду создания нового чата
    std::unique_ptr<NewChatCommand> createNewChatCommand(std::function<void()> callback);
    
    // Создать команду выхода
    std::unique_ptr<ExitCommand> createExitCommand(std::function<void()> callback);
    
    // Создать команду открытия настроек
    std::unique_ptr<OpenSettingsCommand> createOpenSettingsCommand(std::function<void()> callback);
    
    // Создать команду выбора модели
    std::unique_ptr<SelectModelCommand> createSelectModelCommand(std::function<void()> callback);
    
    // Создать команду управления сервером
    std::unique_ptr<ServerControlCommand> createServerControlCommand(
        ServerControlCommand::Action action,
        std::function<void(ServerControlCommand::Action)> callback);
    
    // Создать команду показа помощи
    std::unique_ptr<ShowHelpCommand> createShowHelpCommand(
        std::function<void(const std::string&)> callback,
        const std::string& help_type = "help");
    
    // Создать функциональную команду
    std::unique_ptr<FunctionalCommand> createFunctionalCommand(
        const std::string& name,
        std::function<void()> execute_func,
        const std::string& description = "",
        const std::string& shortcut = "",
        std::function<bool()> can_execute_func = nullptr);
    
    // Создать команду экспорта разговора в JSON
    std::unique_ptr<ExportConversationJsonCommand> createExportConversationJsonCommand(std::function<void()> callback);
    
    // Создать команду установки темы
    std::unique_ptr<SetThemeCommand> createSetThemeCommand(const std::string& theme_name, std::function<void(const std::string&)> callback);
    
    // Создать команду открытия файла
    std::unique_ptr<OpenFileCommand> createOpenFileCommand(std::function<void()> callback);
    
    // Создать команду сохранения файла
    std::unique_ptr<SaveFileCommand> createSaveFileCommand(std::function<void()> callback);
    
    // Создать команду сохранения файла как
    std::unique_ptr<SaveFileAsCommand> createSaveFileAsCommand(std::function<void()> callback);
    
    // Создать команду управления рабочим пространством
    std::unique_ptr<WorkspaceCommand> createWorkspaceCommand(
        WorkspaceCommand::Action action,
        std::function<void(WorkspaceCommand::Action, const std::string&)> callback,
        const std::string& workspace_name = "");
}

// Класс для автоматической регистрации команд
class CommandRegistrar {
private:
    CommandManager* manager_;
    
public:
    explicit CommandRegistrar(CommandManager* manager) : manager_(manager) {}
    
    template<typename CommandType, typename... Args>
    bool registerCommand(const std::string& name, Args&&... args) {
        if (!manager_) return false;
        
        auto command = std::make_unique<CommandType>(std::forward<Args>(args)...);
        return manager_->registerCommand(name, std::move(command));
    }
    
    bool registerShortcut(const std::string& shortcut, const std::string& command_name) {
        if (!manager_) return false;
        return manager_->registerShortcut(shortcut, command_name);
    }
};

} // namespace ui
} // namespace llama_gui