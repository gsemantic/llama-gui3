#pragma once

#include "ui/workspace_manager.h"
#include "settings.h"
#include <string>

namespace llama_gui {
namespace core {

/**
 * @brief Интеграция WorkspaceManager с Settings
 * 
 * Сохраняет текущее рабочее пространство в профиль настроек
 * и загружает его при переключении профилей
 */
class WorkspaceSettingsIntegration {
public:
    WorkspaceSettingsIntegration(Settings& settings, ui::WorkspaceManager& workspace_manager);
    ~WorkspaceSettingsIntegration();

    /**
     * @brief Сохранить текущий workspace в профиль
     */
    void saveWorkspaceToProfile();

    /**
     * @brief Загрузить workspace из профиля
     */
    void loadWorkspaceFromProfile();

    /**
     * @brief Сбросить workspace к значению по умолчанию
     */
    void resetWorkspaceToDefault();

    /**
     * @brief Получить имя текущего workspace из настроек
     */
    std::string getCurrentWorkspaceFromSettings() const;

    /**
     * @brief Установить workspace в настройках
     * @param workspace_name Имя workspace
     */
    void setCurrentWorkspaceInSettings(const std::string& workspace_name);

private:
    Settings& settings_;
    ui::WorkspaceManager& workspace_manager_;

    // Callback при изменении workspace
    void onWorkspaceChanged(ui::WorkspaceType type);
};

} // namespace core
} // namespace llama_gui
