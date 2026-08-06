#pragma once

#include "command_base.h"
#include <string>
#include <functional>

namespace llama_gui {
namespace ui {

// =========================================================================
// Команды управления файлами
// =========================================================================

/**
 * @brief Команда создания нового чата
 */
class NewChatCommand : public Command {
private:
    std::function<void()> callback_;

public:
    explicit NewChatCommand(std::function<void()> callback)
        : callback_(std::move(callback)) {}

    void execute() override {
        if (callback_) {
            callback_();
        }
    }

    std::string getName() const override {
        return "New Chat";
    }

    std::string getDescription() const override {
        return "Create a new conversation";
    }

    std::string getShortcut() const override {
        return "Ctrl+N";
    }
};

/**
 * @brief Команда открытия файла
 */
class OpenFileCommand : public Command {
private:
    std::function<void()> callback_;

public:
    explicit OpenFileCommand(std::function<void()> callback)
        : callback_(std::move(callback)) {}

    void execute() override {
        if (callback_) {
            callback_();
        }
    }

    std::string getName() const override {
        return "Open File";
    }

    std::string getDescription() const override {
        return "Open a file";
    }

    std::string getShortcut() const override {
        return "Ctrl+O";
    }
};

/**
 * @brief Команда сохранения файла
 */
class SaveFileCommand : public Command {
private:
    std::function<void()> callback_;

public:
    explicit SaveFileCommand(std::function<void()> callback)
        : callback_(std::move(callback)) {}

    void execute() override {
        if (callback_) {
            callback_();
        }
    }

    std::string getName() const override {
        return "Save File";
    }

    std::string getDescription() const override {
        return "Save the current file";
    }

    std::string getShortcut() const override {
        return "Ctrl+S";
    }
};

/**
 * @brief Команда сохранения файла как
 */
class SaveFileAsCommand : public Command {
private:
    std::function<void()> callback_;

public:
    explicit SaveFileAsCommand(std::function<void()> callback)
        : callback_(std::move(callback)) {}

    void execute() override {
        if (callback_) {
            callback_();
        }
    }

    std::string getName() const override {
        return "Save File As";
    }

    std::string getDescription() const override {
        return "Save the current file with a new name";
    }
};

/**
 * @brief Команда экспорта разговора в JSON
 */
class ExportConversationJsonCommand : public Command {
private:
    std::function<void()> callback_;

public:
    explicit ExportConversationJsonCommand(std::function<void()> callback)
        : callback_(std::move(callback)) {}

    void execute() override {
        if (callback_) {
            callback_();
        }
    }

    std::string getName() const override {
        return "Export Conversation as JSON";
    }

    std::string getDescription() const override {
        return "Export the current conversation to a JSON file";
    }
};

/**
 * @brief Команда выхода из приложения
 */
class ExitCommand : public Command {
private:
    std::function<void()> callback_;

public:
    explicit ExitCommand(std::function<void()> callback)
        : callback_(std::move(callback)) {}

    void execute() override {
        if (callback_) {
            callback_();
        }
    }

    std::string getName() const override {
        return "Exit";
    }

    std::string getDescription() const override {
        return "Exit the application";
    }

    std::string getShortcut() const override {
        return "Alt+F4";
    }

    bool requiresConfirmation() const override {
        return true;
    }

    std::string getConfirmationText() const override {
        return "Are you sure you want to exit?";
    }
};

} // namespace ui
} // namespace llama_gui
