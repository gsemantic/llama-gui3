#pragma once

#include "command_base.h"
#include <string>
#include <functional>

namespace llama_gui {
namespace ui {

// =========================================================================
// Команды настроек и сервера
// =========================================================================

/**
 * @brief Команда открытия настроек
 */
class OpenSettingsCommand : public Command {
private:
    std::function<void()> callback_;

public:
    explicit OpenSettingsCommand(std::function<void()> callback)
        : callback_(std::move(callback)) {}

    void execute() override {
        if (callback_) {
            callback_();
        }
    }

    std::string getName() const override {
        return "Settings";
    }

    std::string getDescription() const override {
        return "Open application settings";
    }

    std::string getShortcut() const override {
        return "Ctrl+,";
    }
};

/**
 * @brief Команда выбора модели
 */
class SelectModelCommand : public Command {
private:
    std::function<void()> callback_;

public:
    explicit SelectModelCommand(std::function<void()> callback)
        : callback_(std::move(callback)) {}

    void execute() override {
        if (callback_) {
            callback_();
        }
    }

    std::string getName() const override {
        return "Select Model";
    }

    std::string getDescription() const override {
        return "Select a language model";
    }
};

/**
 * @brief Команда управления сервером
 */
class ServerControlCommand : public Command {
public:
    enum class Action {
        Start,
        Stop,
        Restart,
        Status
    };

private:
    Action action_;
    std::function<void(Action)> callback_;

public:
    ServerControlCommand(Action action, std::function<void(Action)> callback)
        : action_(action), callback_(std::move(callback)) {}

    void execute() override {
        if (callback_) {
            callback_(action_);
        }
    }

    std::string getName() const override {
        switch (action_) {
            case Action::Start: return "Start Server";
            case Action::Stop: return "Stop Server";
            case Action::Restart: return "Restart Server";
            case Action::Status: return "Server Status";
            default: return "Server Control";
        }
    }

    std::string getDescription() const override {
        switch (action_) {
            case Action::Start: return "Start the language model server";
            case Action::Stop: return "Stop the language model server";
            case Action::Restart: return "Restart the language model server";
            case Action::Status: return "Show server status information";
            default: return "Server control operation";
        }
    }
};

/**
 * @brief Команда установки темы
 */
class SetThemeCommand : public Command {
private:
    std::string theme_name_;
    std::function<void(const std::string&)> callback_;

public:
    SetThemeCommand(const std::string& theme_name, std::function<void(const std::string&)> callback)
        : theme_name_(theme_name), callback_(std::move(callback)) {}

    void execute() override {
        if (callback_) {
            callback_(theme_name_);
        }
    }

    std::string getName() const override {
        return "Set Theme: " + theme_name_;
    }

    std::string getDescription() const override {
        return "Change the application theme to " + theme_name_;
    }
};

} // namespace ui
} // namespace llama_gui
