#include "../include/core/config_manager.h"
#include <iostream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace llama_gui {
namespace core {

ConfigManager::ConfigManager()
    : owned_settings_(std::make_unique<Settings>())
    , settings_ptr_(owned_settings_.get()) {
}

ConfigManager::~ConfigManager() {
    if (initialized_) {
        saveSettings(false);
    }
}

void ConfigManager::bindExternalSettings(Settings* settings) {
    if (settings) {
        settings_ptr_ = settings;
        owned_settings_.reset();
    }
}

bool ConfigManager::initialize() {
    std::cout << "======================================================" << std::endl;
    std::cout << "  ConfigManager: Инициализация" << std::endl;
    std::cout << "======================================================" << std::endl;

    // Шаг 1: Привязываем Settings к ProfileManager
    profile_manager_.bindSettings(settings_ptr_);
    profile_manager_.setProfilesDirectory(settings_ptr_->get_profiles_directory());

    // Шаг 2: Создаём директории
    if (!createConfigDirectories()) {
        std::cerr << "ConfigManager: Ошибка создания директорий" << std::endl;
        return false;
    }

    // Шаг 3: Валидируем пути
    if (!validateConfigPaths()) {
        std::cerr << "ConfigManager: Ошибка валидации путей" << std::endl;
        return false;
    }

    // Шаг 4: Загружаем профиль напрямую через profile_manager_
    // НЕ используем synchronizeAtStartup() — он создаёт временный Settings!
    std::cout << "\nConfigManager: Загрузка профиля..." << std::endl;
    if (!profile_manager_.loadLastProfile()) {
        std::cerr << "ConfigManager: Профиль не найден, используются значения по умолчанию" << std::endl;
    }

    // Шаг 5: Синхронизируем current_profile_ с ProfileManager
    current_profile_ = profile_manager_.getCurrentProfileName();
    if (!current_profile_.empty()) {
        std::cout << "ConfigManager: Текущий профиль установлен: " << current_profile_ << std::endl;
    }

    // Шаг 6: Валидируем настройки
    if (!validateSettings()) {
        std::cerr << "ConfigManager: Предупреждение: настройки не прошли валидацию" << std::endl;
        std::cerr << getSettingsValidationReport() << std::endl;
    }

    initialized_ = true;
    std::cout << "\n✓ ConfigManager успешно инициализирован" << std::endl;
    std::cout << "======================================================" << std::endl;
    return true;
}

Settings& ConfigManager::getSettings() {
    return *settings_ptr_;
}

const Settings& ConfigManager::getSettings() const {
    return *settings_ptr_;
}

bool ConfigManager::saveSettings(bool sync_profiles) {
    if (!initialized_) {
        std::cerr << "ConfigManager: Не инициализирован" << std::endl;
        return false;
    }

    bool success = true;

    // Сохраняем в профиль (canonical storage)
    if (sync_profiles) {
        std::string profile_name = current_profile_;
        if (profile_name.empty()) {
            profile_name = "default";
        }
        std::cout << "ConfigManager: Сохранение профиля: " << profile_name << std::endl;
        if (!saveProfile(profile_name)) {
            std::cerr << "ConfigManager: Ошибка сохранения профиля" << std::endl;
            success = false;
        }
    }

    if (success) {
        std::cout << "✓ ConfigManager: Настройки сохранены" << std::endl;
    }

    return success;
}

bool ConfigManager::loadProfile(const std::string& profile_name) {
    if (!initialized_) {
        std::cerr << "ConfigManager: Не инициализирован" << std::endl;
        return false;
    }

    std::cout << "ConfigManager: Загрузка профиля: " << profile_name << std::endl;
    if (profile_manager_.loadProfile(profile_name)) {
        current_profile_ = profile_name;
        std::cout << "✓ ConfigManager: Профиль загружен" << std::endl;
        return true;
    }

    std::cerr << "ConfigManager: Ошибка загрузки профиля" << std::endl;
    return false;
}

bool ConfigManager::saveProfile(const std::string& profile_name) {
    if (!initialized_) {
        std::cerr << "ConfigManager: Не инициализирован" << std::endl;
        return false;
    }

    std::string name = profile_name;
    if (name.empty()) {
        name = current_profile_;
        if (name.empty()) {
            name = "default";
        }
    }

    std::cout << "ConfigManager: Сохранение профиля: " << name << std::endl;
    if (profile_manager_.saveProfile(name)) {
        current_profile_ = name;
        std::cout << "✓ ConfigManager: Профиль сохранён" << std::endl;
        return true;
    }

    std::cerr << "ConfigManager: Ошибка сохранения профиля" << std::endl;
    return false;
}

std::vector<std::string> ConfigManager::listProfiles() const {
    return profile_manager_.listProfiles();
}

std::string ConfigManager::getCurrentProfileName() const {
    return current_profile_;
}

ProfileManager& ConfigManager::getProfileManager() {
    return profile_manager_;
}

const ProfileManager& ConfigManager::getProfileManager() const {
    return profile_manager_;
}

bool ConfigManager::resetToDefaults() {
    std::cout << "ConfigManager: Сброс к настройкам по умолчанию..." << std::endl;
    current_profile_.clear();
    return settings_ptr_->reset_to_defaults();
}

std::string ConfigManager::getSettingsIniPath() const {
    return settings_ptr_->get_ini_file_path();
}

std::string ConfigManager::getProfilesDirectory() const {
    return profile_manager_.getProfilesDirectory();
}

void ConfigManager::setProfilesDirectory(const std::string& path) {
    profile_manager_.setProfilesDirectory(path);
    settings_ptr_->set_profiles_directory(path);
}

bool ConfigManager::validateSettings() const {
    return settings_ptr_->validate();
}

std::string ConfigManager::getSettingsValidationReport() const {
    return settings_ptr_->get_validation_errors();
}

std::string ConfigManager::createBackup(const std::string& backup_path) {
    namespace fs = std::filesystem;

    std::string backup_dir = "backups/configs";
    if (!backup_path.empty()) {
        backup_dir = backup_path;
    }

    // Создаём директорию резервных копий
    if (!fs::exists(backup_dir)) {
        fs::create_directories(backup_dir);
    }

    // Генерируем имя файла с временной меткой
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t_now), "%Y%m%d_%H%M%S");
    std::string timestamp = ss.str();

    std::string backup_file = backup_dir + "/settings_backup_" + timestamp + ".ini";

    // Копируем settings.ini
    std::string ini_path = settings_ptr_->get_ini_file_path();
    if (fs::exists(ini_path)) {
        try {
            fs::copy_file(ini_path, backup_file, fs::copy_options::overwrite_existing);
            std::cout << "ConfigManager: Резервная копия создана: " << backup_file << std::endl;
            return backup_file;
        } catch (const std::exception& e) {
            std::cerr << "ConfigManager: Ошибка создания резервной копии: " << e.what() << std::endl;
        }
    }

    return "";
}

bool ConfigManager::restoreFromBackup(const std::string& backup_path) {
    namespace fs = std::filesystem;

    if (!fs::exists(backup_path)) {
        std::cerr << "ConfigManager: Файл резервной копии не найден: " << backup_path << std::endl;
        return false;
    }

    std::string ini_path = settings_ptr_->get_ini_file_path();
    try {
        fs::copy_file(backup_path, ini_path, fs::copy_options::overwrite_existing);
        std::cout << "ConfigManager: Восстановление из резервной копии: " << backup_path << std::endl;
        return settings_ptr_->load_from_ini(ini_path);
    } catch (const std::exception& e) {
        std::cerr << "ConfigManager: Ошибка восстановления: " << e.what() << std::endl;
        return false;
    }
}

std::string ConfigManager::getDebugInfo() const {
    std::stringstream ss;

    ss << "======================================================\n";
    ss << "  ConfigManager Debug Info\n";
    ss << "======================================================\n";
    ss << "Initialized: " << (initialized_ ? "yes" : "no") << "\n";
    ss << "Current profile: " << (current_profile_.empty() ? "(none)" : current_profile_) << "\n";
    ss << "Settings INI path: " << settings_ptr_->get_ini_file_path() << "\n";
    ss << "Profiles directory: " << profile_manager_.getProfilesDirectory() << "\n";

    auto profiles = profile_manager_.listProfiles();
    ss << "Available profiles: " << profiles.size() << "\n";
    for (const auto& p : profiles) {
        ss << "  - " << p << "\n";
    }

    ss << "\n";
    ss << settings_ptr_->get_debug_info();

    return ss.str();
}

bool ConfigManager::createConfigDirectories() {
    namespace fs = std::filesystem;

    std::cout << "ConfigManager: Создание директорий..." << std::endl;

    // Директория профилей
    std::string profiles_dir = profile_manager_.getProfilesDirectory();
    if (!fs::exists(profiles_dir)) {
        std::cout << "  Создаём директорию профилей: " << profiles_dir << std::endl;
        fs::create_directories(profiles_dir);
    }

    // Директория для INI файла
    std::string ini_path = settings_ptr_->get_ini_file_path();
    std::string ini_dir = ini_path.substr(0, ini_path.find_last_of('/'));
    if (!ini_dir.empty() && !fs::exists(ini_dir)) {
        std::cout << "  Создаём директорию настроек: " << ini_dir << std::endl;
        fs::create_directories(ini_dir);
    }

    // Директория резервных копий
    std::string backup_dir = "backups/configs";
    if (!fs::exists(backup_dir)) {
        std::cout << "  Создаём директорию резервных копий: " << backup_dir << std::endl;
        fs::create_directories(backup_dir);
    }

    return true;
}

bool ConfigManager::validateConfigPaths() const {
    // Проверяем, что пути не пустые
    if (settings_ptr_->get_ini_file_path().empty()) {
        std::cerr << "ConfigManager: Путь к settings.ini пустой" << std::endl;
        return false;
    }

    if (profile_manager_.getProfilesDirectory().empty()) {
        std::cerr << "ConfigManager: Путь к директории профилей пустой" << std::endl;
        return false;
    }

    return true;
}

} // namespace core
} // namespace llama_gui
