#include "../include/core/profile_manager.h"
#include "../include/core/settings.h"
#include "../include/ui/workspace_manager.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace llama_gui {
namespace core {

using json = nlohmann::json;

ProfileManager::ProfileManager() = default;
ProfileManager::~ProfileManager() = default;

// =========================================================================
// Инициализация
// =========================================================================

void ProfileManager::bindSettings(Settings* settings) {
    settings_ = settings;
}

void ProfileManager::bindWorkspaceManager(ui::WorkspaceManager* workspace_manager) {
    workspace_manager_ = workspace_manager;
}

void ProfileManager::setProfilesDirectory(const std::string& path) {
    profiles_directory_ = path;
}

std::string ProfileManager::getProfilesDirectory() const {
    return profiles_directory_;
}

// =========================================================================
// CRUD операции
// =========================================================================

std::vector<std::string> ProfileManager::listProfiles() const {
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

bool ProfileManager::loadProfile(const std::string& profile_name) {
    if (profile_name.empty() || !settings_) {
        return false;
    }

    std::string path = getProfilePath(profile_name);
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[ProfileManager] Не удалось открыть файл: " << path << std::endl;
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string raw_json = buffer.str();

    // Нормализуем JSON перед загрузкой (убираем дубли)
    std::string normalized = resolveDuplicates(raw_json);

    if (settings_->deserialize_from_json(normalized)) {
        current_profile_name_ = profile_name;

        // Применяем workspace конфигурацию из профиля
        applyWorkspaceFromJson(normalized);

        std::cout << "[ProfileManager] Профиль загружен: " << profile_name << std::endl;
        return true;
    }

    std::cerr << "[ProfileManager] Ошибка десериализации профиля: " << profile_name << std::endl;
    return false;
}

bool ProfileManager::saveProfile(const std::string& profile_name) {
    std::string name = profile_name;
    if (name.empty()) {
        name = current_profile_name_;
    }
    if (name.empty() || !settings_) {
        return false;
    }

    if (!std::filesystem::exists(profiles_directory_)) {
        std::filesystem::create_directories(profiles_directory_);
    }

    // Сериализуем настройки
    std::string settings_json = settings_->serialize_to_json();

    // Встраиваем workspace конфигурацию
    std::string workspace_json = getWorkspaceJson();
    std::string final_json;
    if (!workspace_json.empty()) {
        try {
            json j = json::parse(settings_json);
            json ws = json::parse(workspace_json);
            j["workspace"] = ws;
            final_json = j.dump();
        } catch (const std::exception& e) {
            std::cerr << "[ProfileManager] Ошибка встраивания workspace: " << e.what() << std::endl;
            final_json = settings_json;
        }
    } else {
        final_json = settings_json;
    }

    std::string path = getProfilePath(name);
    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "[ProfileManager] Не удалось открыть файл для записи: " << path << std::endl;
        return false;
    }

    file << final_json;
    current_profile_name_ = name;

    std::cout << "[ProfileManager] Профиль сохранён: " << name << std::endl;
    return true;
}

bool ProfileManager::deleteProfile(const std::string& profile_name) {
    if (profile_name.empty()) {
        return false;
    }

    std::string path = getProfilePath(profile_name);
    if (std::filesystem::exists(path)) {
        bool removed = std::filesystem::remove(path);
        if (removed && current_profile_name_ == profile_name) {
            current_profile_name_.clear();
        }
        return removed;
    }
    return false;
}

bool ProfileManager::renameProfile(const std::string& old_name, const std::string& new_name) {
    if (old_name.empty() || new_name.empty()) {
        return false;
    }

    std::string old_path = getProfilePath(old_name);
    std::string new_path = getProfilePath(new_name);

    if (!std::filesystem::exists(old_path)) {
        return false;
    }

    try {
        std::filesystem::copy_file(old_path, new_path, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::remove(old_path);

        if (current_profile_name_ == old_name) {
            current_profile_name_ = new_name;
        }

        std::cout << "[ProfileManager] Профиль переименован: " << old_name << " → " << new_name << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[ProfileManager] Ошибка переименования: " << e.what() << std::endl;
        return false;
    }
}

bool ProfileManager::loadLastProfile() {
    std::vector<std::string> profiles = listProfiles();
    if (profiles.empty()) {
        return false;
    }

    std::string last_profile;
    std::filesystem::file_time_type last_mtime;

    for (const auto& profile : profiles) {
        std::string path = getProfilePath(profile);
        try {
            auto mtime = std::filesystem::last_write_time(path);
            if (last_profile.empty() || mtime > last_mtime) {
                last_mtime = mtime;
                last_profile = profile;
            }
        } catch (const std::exception& e) {
            std::cerr << "[ProfileManager] Ошибка чтения времени модификации " << path << ": " << e.what() << std::endl;
        }
    }

    if (!last_profile.empty()) {
        return loadProfile(last_profile);
    }
    return false;
}

// =========================================================================
// Текущий профиль
// =========================================================================

std::string ProfileManager::getCurrentProfileName() const {
    return current_profile_name_;
}

void ProfileManager::setCurrentProfileName(const std::string& name) {
    current_profile_name_ = name;
}

// =========================================================================
// Синхронизация
// =========================================================================

bool ProfileManager::synchronizeAtStartup() {
    std::cout << "[ProfileManager] Синхронизация при старте..." << std::endl;

    bool has_profiles = !listProfiles().empty();

    // Приоритет 1: Последний профиль
    if (has_profiles) {
        if (loadLastProfile()) {
            std::cout << "[ProfileManager] Загружен последний профиль: " << current_profile_name_ << std::endl;
            return true;
        }
    }

    // Приоритет 2: Настройки по умолчанию
    if (settings_) {
        settings_->reset_to_defaults();
    }
    std::cout << "[ProfileManager] Используются настройки по умолчанию" << std::endl;
    return false;
}

// =========================================================================
// Приватные методы
// =========================================================================

std::string ProfileManager::getProfilePath(const std::string& profile_name) const {
    return profiles_directory_ + "/" + profile_name + ".json";
}

// =========================================================================
// Workspace интеграция (Phase 4)
// =========================================================================

void ProfileManager::applyWorkspaceFromJson(const std::string& json_str) {
    if (!workspace_manager_) {
        return;
    }

    try {
        json j = json::parse(json_str);
        if (j.contains("workspace")) {
            std::string ws_data = j["workspace"].dump();
            workspace_manager_->deserializeFromJson(ws_data);
            std::cout << "[ProfileManager] Workspace конфигурация применена из профиля" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[ProfileManager] Ошибка применения workspace: " << e.what() << std::endl;
    }
}

std::string ProfileManager::getWorkspaceJson() const {
    if (!workspace_manager_) {
        return "";
    }

    try {
        return workspace_manager_->serializeToJson();
    } catch (const std::exception& e) {
        std::cerr << "[ProfileManager] Ошибка сериализации workspace: " << e.what() << std::endl;
        return "";
    }
}

// =========================================================================
// Нормализация JSON (Phase 2)
// =========================================================================

std::string ProfileManager::resolveDuplicates(const std::string& json_str) {
    try {
        json j = json::parse(json_str);
        bool modified = false;

        // --- model_path: model_loading побеждает custom ---
        if (j.contains("model_loading") && j["model_loading"].contains("model_path")) {
            std::string ml_path = j["model_loading"]["model_path"].get<std::string>();
            if (!ml_path.empty() && j.contains("custom") && j["custom"].contains("model_path")) {
                std::string custom_path = j["custom"]["model_path"].get<std::string>();
                if (custom_path != ml_path) {
                    std::cout << "[ProfileManager] resolveDuplicates: custom.model_path != model_loading.model_path, "
                              << "uses model_loading" << std::endl;
                }
                j["custom"]["model_path"] = ml_path;
                modified = true;
            }
        }

        // --- temperature: sampling побеждает chat ---
        if (j.contains("sampling") && j.contains("chat")) {
            float sampling_temp = j["sampling"].value("temperature", 0.7f);
            float chat_temp = j["chat"].value("temperature", 0.7f);
            if (std::abs(sampling_temp - chat_temp) > 0.001f) {
                std::cout << "[ProfileManager] resolveDuplicates: chat.temperature=" << chat_temp
                          << " != sampling.temperature=" << sampling_temp
                          << ", uses sampling" << std::endl;
                j["chat"]["temperature"] = sampling_temp;
                modified = true;
            }
        }

        // --- repeat_penalty: sampling побеждает chat ---
        if (j.contains("sampling") && j.contains("chat")) {
            float sampling_rp = j["sampling"].value("repeat_penalty", 1.0f);
            float chat_rp = j["chat"].value("repeat_penalty", 1.1f);
            if (std::abs(sampling_rp - chat_rp) > 0.001f) {
                std::cout << "[ProfileManager] resolveDuplicates: chat.repeat_penalty=" << chat_rp
                          << " != sampling.repeat_penalty=" << sampling_rp
                          << ", uses sampling" << std::endl;
                j["chat"]["repeat_penalty"] = sampling_rp;
                modified = true;
            }
        }

        // --- n_ctx: chat побеждает batch ---
        if (j.contains("batch") && j.contains("chat")) {
            int batch_ctx = j["batch"].value("ctx_size", 4096);
            int chat_ctx = j["chat"].value("n_ctx", 4096);
            if (batch_ctx != chat_ctx) {
                std::cout << "[ProfileManager] resolveDuplicates: batch.ctx_size=" << batch_ctx
                          << " != chat.n_ctx=" << chat_ctx
                          << ", uses chat.n_ctx" << std::endl;
                j["batch"]["ctx_size"] = chat_ctx;
                modified = true;
            }
        }

        // --- threads: batch побеждает chat ---
        if (j.contains("batch") && j.contains("chat")) {
            int batch_threads = j["batch"].value("threads", 2);
            int chat_threads = j["chat"].value("threads", 4);
            if (batch_threads != chat_threads) {
                std::cout << "[ProfileManager] resolveDuplicates: chat.threads=" << chat_threads
                          << " != batch.threads=" << batch_threads
                          << ", uses batch.threads" << std::endl;
                j["chat"]["threads"] = batch_threads;
                modified = true;
            }
        }

        // --- n_gpu_layers: gpu побеждает chat ---
        if (j.contains("gpu") && j.contains("chat")) {
            int gpu_layers = j["gpu"].value("n_gpu_layers", 0);
            int chat_layers = j["chat"].value("n_gpu_layers", 0);
            if (gpu_layers >= 0 && gpu_layers != chat_layers) {
                std::cout << "[ProfileManager] resolveDuplicates: chat.n_gpu_layers=" << chat_layers
                          << " != gpu.n_gpu_layers=" << gpu_layers
                          << ", uses gpu" << std::endl;
                j["chat"]["n_gpu_layers"] = gpu_layers;
                modified = true;
            }
        }

        if (modified) {
            return j.dump();
        }
    } catch (const std::exception& e) {
        std::cerr << "[ProfileManager] resolveDuplicates error: " << e.what() << std::endl;
    }

    // Если не удалось нормализовать или нечего менять — возвращаем оригинал
    return json_str;
}

} // namespace core
} // namespace llama_gui
