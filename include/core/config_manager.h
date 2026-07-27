#pragma once

#include <string>
#include <memory>
#include <functional>
#include "settings.h"
#include "profile_manager.h"

namespace llama_gui {
namespace core {

/**
 * @class ConfigManager
 * @brief Единый центр управления конфигурацией приложения
 *
 * Управляет всеми источниками настроек:
 * - settings.ini (основной файл настроек)
 * - profiles/*.json (профили настроек) — через ProfileManager
 * - workspaces_config.json (рабочие пространства)
 *
 * ПРИОРИТЕТ НАСТРОЕК (от высшего к низшему):
 * 1. Command line arguments (аргументы командной строки)
 * 2. profiles/*.json (последний изменённый профиль)
 * 3. settings.ini (основной файл настроек)
 * 4. Настройки по умолчанию
 *
 * Пример использования:
 * @code
 * ConfigManager config;
 * config.initialize();
 * auto& settings = config.getSettings();
 * @endcode
 */
class ConfigManager {
public:
    ConfigManager();
    ~ConfigManager();

    // Запрет копирования
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    /**
     * @brief Привязать внешний Settings (из main.cpp)
     *
     * Если не вызван — ConfigManager создаёт свой Settings.
     * Если вызван — использует переданный объект.
     */
    void bindExternalSettings(Settings* settings);

    /**
     * @brief Инициализация менеджера конфигурации
     *
     * Выполняет:
     * 1. Создание директорий для конфигов
     * 2. Синхронизацию настроек (profiles > settings.ini > defaults)
     * 3. Валидацию настроек
     */
    bool initialize();

    /**
     * @brief Получить доступ к настройкам
     */
    Settings& getSettings();
    const Settings& getSettings() const;

    /**
     * @brief Получить доступ к менеджеру профилей
     */
    ProfileManager& getProfileManager();
    const ProfileManager& getProfileManager() const;

    /**
     * @brief Сохранить текущие настройки
     * @param sync_profiles если true, сохранить и в profiles тоже
     * @return true если успешно
     */
    bool saveSettings(bool sync_profiles = true);

    /**
     * @brief Загрузить настройки из указанного профиля
     * @param profile_name имя профиля
     * @return true если успешно
     */
    bool loadProfile(const std::string& profile_name);

    /**
     * @brief Сохранить текущие настройки в профиль
     * @param profile_name имя профиля (пустое = текущий)
     * @return true если успешно
     */
    bool saveProfile(const std::string& profile_name = "");

    /**
     * @brief Получить список доступных профилей
     */
    std::vector<std::string> listProfiles() const;

    /**
     * @brief Получить имя текущего профиля
     */
    std::string getCurrentProfileName() const;

    /**
     * @brief Сбросить настройки к значениям по умолчанию
     */
    bool resetToDefaults();

    /**
     * @brief Получить путь к основному файлу настроек
     */
    std::string getSettingsIniPath() const;

    /**
     * @brief Получить путь к директории профилей
     */
    std::string getProfilesDirectory() const;

    /**
     * @brief Установить путь к директории профилей
     */
    void setProfilesDirectory(const std::string& path);

    /**
     * @brief Проверить валидность текущих настроек
     * @return true если настройки валидны
     */
    bool validateSettings() const;

    /**
     * @brief Получить отчёт о валидации настроек
     */
    std::string getSettingsValidationReport() const;

    /**
     * @brief Создать резервную копию настроек
     * @param backup_path путь для сохранения (пустой = авто)
     * @return путь к файлу резервной копии или пустая строка при ошибке
     */
    std::string createBackup(const std::string& backup_path = "");

    /**
     * @brief Восстановить настройки из резервной копии
     * @param backup_path путь к файлу резервной копии
     * @return true если успешно
     */
    bool restoreFromBackup(const std::string& backup_path);

    /**
     * @brief Получить информацию о конфигурации для отладки
     */
    std::string getDebugInfo() const;

private:
    Settings* settings_ptr_ = nullptr;
    std::unique_ptr<Settings> owned_settings_;
    ProfileManager profile_manager_;
    std::string current_profile_;
    bool initialized_ = false;

    /**
     * @brief Создать необходимые директории
     */
    bool createConfigDirectories();

    /**
     * @brief Валидировать пути к конфигам
     */
    bool validateConfigPaths() const;
};

} // namespace core
} // namespace llama_gui
