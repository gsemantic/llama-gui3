#pragma once

#include "command_base.h"
#include "localization_manager.h"
#include <string>
#include <functional>

namespace llama_gui {
namespace ui {

// =========================================================================
// Команды помощи и локализации
// =========================================================================

/**
 * @brief Команда показа диалога помощи
 */
class ShowHelpCommand : public Command {
private:
    std::function<void(const std::string&)> callback_;
    std::string help_type_;

public:
    ShowHelpCommand(std::function<void(const std::string&)> callback, const std::string& help_type = "help")
        : callback_(std::move(callback)), help_type_(help_type) {}

    void execute() override {
        if (callback_) {
            callback_(help_type_);
        }
    }

    std::string getName() const override {
        if (help_type_ == "about") return "About";
        if (help_type_ == "shortcuts") return "Keyboard Shortcuts";
        return "Help";
    }

    std::string getDescription() const override {
        if (help_type_ == "about") return "Show information about the application";
        if (help_type_ == "shortcuts") return "Show keyboard shortcuts";
        return "Show help information";
    }
};

/**
 * @brief Команда переключения языка
 */
class ToggleLanguageCommand : public Command {
private:
    std::function<void()> callback_;
    Language target_language_;

public:
    ToggleLanguageCommand(std::function<void()> callback, Language target_language = Language::English)
        : callback_(std::move(callback)), target_language_(target_language) {}

    void execute() override {
        if (callback_) {
            callback_();
        }
    }

    std::string getName() const override {
        return target_language_ == Language::Russian ? "Switch to Russian" : "Switch to English";
    }

    std::string getDescription() const override {
        return target_language_ == Language::Russian ? "Switch interface to Russian"
                                                     : "Switch interface to English";
    }

    std::string getShortcut() const override {
        return "Ctrl+L";
    }
};

} // namespace ui
} // namespace llama_gui
