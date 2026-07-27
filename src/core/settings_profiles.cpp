#include "../include/core/settings.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <filesystem>

namespace llama_gui {
namespace core {

// =========================================================================
// Синхронизация настроек при старте
// =========================================================================

bool Settings::synchronize_at_startup() {
    std::cout << "======================================================" << std::endl;
    std::cout << "  Синхронизация настроек при старте" << std::endl;
    std::cout << "======================================================" << std::endl;

    bool ini_exists = std::filesystem::exists(ini_file_path_);
    std::vector<std::string> profiles = list_profiles();
    bool has_profiles = !profiles.empty();

    std::cout << "settings.ini существует: " << (ini_exists ? "да" : "нет") << std::endl;
    std::cout << "Профили найдены: " << (has_profiles ? "да (" + std::to_string(profiles.size()) + ")" : "нет") << std::endl;

    // Приоритет 1: Последний профиль (если есть)
    if (has_profiles) {
        std::cout << "\n[ПРИОРИТЕТ 1] Загрузка последнего профиля..." << std::endl;
        if (load_last_profile()) {
            std::cout << "✓ Настройки загружены из профиля: " << current_profile_name_ << std::endl;
            std::cout << "✓ settings.ini обновлён синхронизированными значениями" << std::endl;
            return true;
        }
    }

    // Приоритет 2: settings.ini (если есть)
    if (ini_exists) {
        std::cout << "\n[ПРИОРИТЕТ 2] Загрузка из settings.ini..." << std::endl;
        if (load_from_ini(ini_file_path_)) {
            std::cout << "✓ Настройки загружены из settings.ini" << std::endl;
            // Сохраняем в профиль для консистентности
            std::cout << "✓ Создаём профиль по умолчанию для консистентности..." << std::endl;
            save_profile("default");
            return true;
        }
    }

    // Приоритет 3: Настройки по умолчанию
    std::cout << "\n[ПРИОРИТЕТ 3] Используем настройки по умолчанию" << std::endl;
    reset_to_defaults();
    return false;
}

// =========================================================================
// Profile management
// =========================================================================
std::vector<std::string> Settings::list_profiles() const {
    std::vector<std::string> profiles;
    if (!std::filesystem::exists(profiles_directory_)) {
        return profiles;
    }

    for (const auto& entry : std::filesystem::directory_iterator(profiles_directory_)) {
        if (entry.path().extension() == ".json") {
            profiles.push_back(entry.path().stem().string());
        }
    }
    return profiles;
}

bool Settings::save_profile(const std::string& profile_name) const {
    if (profile_name.empty()) return false;

    if (!std::filesystem::exists(profiles_directory_)) {
        std::filesystem::create_directories(profiles_directory_);
    }

    std::string path = profiles_directory_ + "/" + profile_name + ".json";
    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "Failed to open profile file for writing: " << path << std::endl;
        return false;
    }

    std::cout << "save_profile: Сохранение профиля: " << profile_name 
              << " (n_ctx=" << chat_settings_.n_ctx << ", ctx_size=" << batch_settings_.ctx_size << ")" << std::endl;
    
    file << serialize_to_json();
    return true;
}

bool Settings::load_profile(const std::string& profile_name) {
    if (profile_name.empty()) return false;

    std::string path = profiles_directory_ + "/" + profile_name + ".json";
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Failed to open profile file for reading: " << path << std::endl;
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    if (deserialize_from_json(buffer.str())) {
        current_profile_name_ = profile_name;

        std::cout << "load_profile: Загружен профиль: " << profile_name 
                  << " (n_ctx=" << chat_settings_.n_ctx << ", ctx_size=" << batch_settings_.ctx_size << ")" << std::endl;

        // Синхронизируем n_ctx и ctx_size после загрузки профиля
        sync_ctx_size();

        // Синхронизируем max_tokens и n_predict после загрузки профиля
        sync_max_tokens();

        // Сохраняем синхронизированные значения в settings.ini
        std::cout << "load_profile: Сохранение синхронизированных значений в settings.ini" << std::endl;
        save_to_ini(ini_file_path_);

        return true;
    }
    return false;
}

void Settings::sync_ctx_size() {
    // Синхронизируем ctx_size (для сервера) с n_ctx (для GUI)
    // Приоритет отдаётся n_ctx из chat_settings, так как это основной параметр
    std::cout << "sync_ctx_size: Проверка синхронизации... n_ctx=" << chat_settings_.n_ctx 
              << ", ctx_size=" << batch_settings_.ctx_size << std::endl;
    
    if (chat_settings_.n_ctx != batch_settings_.ctx_size) {
        // Если они различаются, устанавливаем batch.ctx_size равным chat.n_ctx
        std::cout << "sync_ctx_size: Синхронизация ctx_size с n_ctx = " << chat_settings_.n_ctx << std::endl;
        batch_settings_.ctx_size = chat_settings_.n_ctx;
        std::cout << "sync_ctx_size: После синхронизации ctx_size = " << batch_settings_.ctx_size << std::endl;
    } else {
        std::cout << "sync_ctx_size: Значения уже синхронизированы" << std::endl;
    }
}

void Settings::sync_max_tokens() {
    // Синхронизация max_tokens (chat) и n_predict (output)
    // Вызывается только если включена синхронизация (sync_max_tokens_enabled_)
    if (!sync_max_tokens_enabled_) {
        return;
    }

    int& max_tokens = chat_settings_.max_tokens;
    int& n_predict = output_settings_.n_predict;

    // Логика синхронизации:
    // ПРИОРИТЕТ У chat.max_tokens - это основной параметр, который редактирует пользователь
    // 1. Если max_tokens != -1, установить n_predict = max_tokens
    // 2. Если max_tokens == -1 (неограниченно), установить n_predict = -1

    if (max_tokens == -1) {
        // max_tokens неограничен, устанавливаем n_predict = -1
        n_predict = -1;
        std::cout << "sync_max_tokens: max_tokens = -1, n_predict установлен в -1 (неограниченно)" << std::endl;
    } else {
        // Устанавливаем n_predict равным max_tokens (приоритет у max_tokens)
        if (n_predict != max_tokens) {
            std::cout << "sync_max_tokens: n_predict синхронизирован с max_tokens = " << max_tokens << std::endl;
        } else {
            std::cout << "sync_max_tokens: значения уже синхронизированы (max_tokens = n_predict = "
                      << max_tokens << ")" << std::endl;
        }
        n_predict = max_tokens;
    }
}

bool Settings::load_last_profile() {
    std::vector<std::string> profiles = list_profiles();
    if (profiles.empty()) {
        std::cout << "No profiles found, using default settings" << std::endl;
        return false;
    }

    // Find the most recently modified profile
    std::string last_profile;
    std::filesystem::file_time_type last_mtime;

    for (const auto& profile : profiles) {
        std::string path = profiles_directory_ + "/" + profile + ".json";
        try {
            auto mtime = std::filesystem::last_write_time(path);
            if (last_profile.empty() || mtime > last_mtime) {
                last_mtime = mtime;
                last_profile = profile;
            }
        } catch (const std::exception& e) {
            std::cerr << "Failed to get modification time for " << path << ": " << e.what() << std::endl;
        }
    }

    if (!last_profile.empty()) {
        std::cout << "Loading last modified profile: " << last_profile << std::endl;
        current_profile_name_ = last_profile;
        return load_profile(last_profile);
    }

    return false;
}

bool Settings::delete_profile(const std::string& profile_name) {
    if (profile_name.empty()) return false;

    std::string path = profiles_directory_ + "/" + profile_name + ".json";
    if (std::filesystem::exists(path)) {
        return std::filesystem::remove(path);
    }
    return false;
}

} // namespace core
} // namespace llama_gui
