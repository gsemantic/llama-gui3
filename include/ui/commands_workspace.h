#pragma once

#include "command_base.h"
#include <string>
#include <functional>

namespace llama_gui {
namespace ui {

// =========================================================================
// Команды рабочей области и режима разработчика
// =========================================================================

/**
 * @brief Команда управления рабочим пространством
 */
class WorkspaceCommand : public Command {
public:
    enum class Action {
        Save,
        Load,
        Reset,
        SwitchToUser,
        SwitchToDeveloper,
        SwitchToAdmin
    };

private:
    Action action_;
    std::string workspace_name_;
    std::function<void(Action, const std::string&)> callback_;

public:
    WorkspaceCommand(Action action, std::function<void(Action, const std::string&)> callback, const std::string& workspace_name = "")
        : action_(action), callback_(std::move(callback)), workspace_name_(workspace_name) {}

    void execute() override {
        if (callback_) {
            callback_(action_, workspace_name_);
        }
    }

    std::string getName() const override {
        switch (action_) {
            case Action::Save: return "Save Workspace";
            case Action::Load: return "Load Workspace";
            case Action::Reset: return "Reset Workspace";
            case Action::SwitchToUser: return "Switch to User Workspace";
            case Action::SwitchToDeveloper: return "Switch to Developer Workspace";
            case Action::SwitchToAdmin: return "Switch to Admin Workspace";
            default: return "Workspace Action";
        }
    }

    std::string getDescription() const override {
        switch (action_) {
            case Action::Save: return "Save the current workspace layout";
            case Action::Load: return "Load a saved workspace layout";
            case Action::Reset: return "Reset workspace to default layout";
            case Action::SwitchToUser: return "Switch to User workspace mode";
            case Action::SwitchToDeveloper: return "Switch to Developer workspace mode";
            case Action::SwitchToAdmin: return "Switch to Admin workspace mode";
            default: return "Workspace management operation";
        }
    }
};

/**
 * @brief Команда переключения в режим разработчика
 */
class ToggleDeveloperModeCommand : public Command {
private:
    std::function<void(bool)> callback_;
    bool enabled_;

public:
    ToggleDeveloperModeCommand(std::function<void(bool)> callback, bool enabled = false)
        : callback_(std::move(callback)), enabled_(enabled) {}

    void execute() override {
        enabled_ = !enabled_;
        if (callback_) {
            callback_(enabled_);
        }
    }

    std::string getName() const override {
        return enabled_ ? "Disable Developer Mode" : "Enable Developer Mode";
    }

    std::string getDescription() const override {
        return enabled_ ? "Disable developer mode and hide advanced tools"
                        : "Enable developer mode to access debugging tools";
    }

    bool requiresConfirmation() const override {
        return !enabled_; // Подтверждение при включении
    }

    std::string getConfirmationText() const override {
        return "Developer mode will show advanced debugging tools. Continue?";
    }
};

} // namespace ui
} // namespace llama_gui
