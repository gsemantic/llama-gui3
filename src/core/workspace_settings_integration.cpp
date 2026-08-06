#include "../include/core/workspace_settings_integration.h"
#include "../include/core/logger.h"
#include <iostream>

namespace llama_gui {
namespace core {

WorkspaceSettingsIntegration::WorkspaceSettingsIntegration(
    Settings& settings, 
    ui::WorkspaceManager& workspace_manager
)
    : settings_(settings)
    , workspace_manager_(workspace_manager) 
{
    // Добавляем callback для автосохранения при переключении workspace
    workspace_manager_.addWorkspaceChangedCallback(
        [this](ui::WorkspaceType type) {
            onWorkspaceChanged(type);
        }
    );
}

WorkspaceSettingsIntegration::~WorkspaceSettingsIntegration() = default;

void WorkspaceSettingsIntegration::saveWorkspaceToProfile() {
    std::string workspace_name = workspace_manager_.getCurrentWorkspaceName();
    setCurrentWorkspaceInSettings(workspace_name);
    
    std::cout << "WorkspaceSettings: Saved workspace '" << workspace_name 
              << "' to profile '" << settings_.get_current_profile_name() << "'" << std::endl;
}

void WorkspaceSettingsIntegration::loadWorkspaceFromProfile() {
    std::string workspace_name = getCurrentWorkspaceFromSettings();
    
    if (!workspace_name.empty()) {
        if (workspace_manager_.switchWorkspace(workspace_name)) {
            std::cout << "WorkspaceSettings: Loaded workspace '" << workspace_name 
                      << "' from profile" << std::endl;
        } else {
            std::cerr << "WorkspaceSettings: Failed to switch to workspace '" 
                      << workspace_name << "'" << std::endl;
        }
    } else {
        // Если workspace не указан в профиле - используем User по умолчанию
        workspace_manager_.switchWorkspace(ui::WorkspaceType::User);
        std::cout << "WorkspaceSettings: Using default User workspace" << std::endl;
    }
}

void WorkspaceSettingsIntegration::resetWorkspaceToDefault() {
    workspace_manager_.switchWorkspace(ui::WorkspaceType::User);
    setCurrentWorkspaceInSettings("User");
    std::cout << "WorkspaceSettings: Reset to default User workspace" << std::endl;
}

std::string WorkspaceSettingsIntegration::getCurrentWorkspaceFromSettings() const {
    // Используем custom settings для хранения имени workspace
    return settings_.get_custom_setting("workspace.current", "User");
}

void WorkspaceSettingsIntegration::setCurrentWorkspaceInSettings(const std::string& workspace_name) {
    settings_.set_custom_setting("workspace.current", workspace_name);
}

void WorkspaceSettingsIntegration::onWorkspaceChanged(ui::WorkspaceType type) {
    // Автосохранение имени workspace в настройках
    std::string workspace_name = ui::workspace_type_to_string(type);
    setCurrentWorkspaceInSettings(workspace_name);
    
    std::cout << "[INFO] Workspace changed to: " << workspace_name << std::endl;
}

} // namespace core
} // namespace llama_gui
