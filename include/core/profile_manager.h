#pragma once

#include <string>
#include <vector>
#include <functional>

namespace llama_gui {
namespace core {

class Settings;

} // namespace core

namespace ui {

class WorkspaceManager;

} // namespace ui

namespace core {

/**
 * @class ProfileManager
 * @brief Единый менеджер профилей настроек
 *
 * Заменяет дублирующийся код из Settings, ConfigManager и ProfileAdapter.
 * Все операции CRUD с профилями проходят через этот класс.
 *
 * Поток данных:
 *   ProfileManager::loadProfile(name)
 *     → читает profiles/<name>.json
 *     → нормализует JSON (resolveDuplicates)
 *     → извлекает секцию "workspace" → передаёт в WorkspaceManager
 *     → вызывает Settings::deserialize_from_json()
 *     → обновляет current_profile_name_
 *
 *   ProfileManager::saveProfile(name)
 *     → получает данные workspace от WorkspaceManager
 *     → встраивает секцию "workspace" в JSON
 *     → вызывает Settings::serialize_to_json()
 *     → пишет в profiles/<name>.json
 */
class ProfileManager {
public:
    ProfileManager();
    ~ProfileManager();

    ProfileManager(const ProfileManager&) = delete;
    ProfileManager& operator=(const ProfileManager&) = delete;

    // =========================================================================
    // Инициализация
    // =========================================================================

    void bindSettings(Settings* settings);
    void bindWorkspaceManager(ui::WorkspaceManager* workspace_manager);
    void setProfilesDirectory(const std::string& path);
    std::string getProfilesDirectory() const;

    // =========================================================================
    // CRUD операции
    // =========================================================================

    std::vector<std::string> listProfiles() const;
    bool loadProfile(const std::string& profile_name);
    bool saveProfile(const std::string& profile_name = "");
    bool deleteProfile(const std::string& profile_name);
    bool renameProfile(const std::string& old_name, const std::string& new_name);
    bool loadLastProfile();

    // =========================================================================
    // Текущий профиль
    // =========================================================================

    std::string getCurrentProfileName() const;
    void setCurrentProfileName(const std::string& name);

    // =========================================================================
    // Синхронизация
    // =========================================================================

    bool synchronizeAtStartup();

    // =========================================================================
    // Нормализация JSON (Phase 2)
    // =========================================================================

    static std::string resolveDuplicates(const std::string& json_str);

private:
    Settings* settings_ = nullptr;
    ui::WorkspaceManager* workspace_manager_ = nullptr;
    std::string profiles_directory_ = "profiles";
    std::string current_profile_name_;

    std::string getProfilePath(const std::string& profile_name) const;

    /**
     * @brief Извлечь секцию workspace из JSON и применить к WorkspaceManager
     */
    void applyWorkspaceFromJson(const std::string& json_str);

    /**
     * @brief Получить JSON секции workspace от WorkspaceManager
     */
    std::string getWorkspaceJson() const;
};

} // namespace core
} // namespace llama_gui
