#include "../../include/core/settings_snapshot.h"
#include "../../include/core/config_manager.h"
#include "../../include/core/env_manager.h"
#include "../../include/core/version.h"

#include <nlohmann/json.hpp>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace llama_gui {
namespace core {

using json = nlohmann::json;

namespace {

constexpr const char* kSnapshotFormat = "llama-gui-settings-snapshot";
constexpr int kSnapshotVersion = 1;

std::string utc_timestamp_now() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    std::stringstream ss;
    ss << std::put_time(&tm_utc, "%Y-%m-%d %H:%M:%S UTC");
    return ss.str();
}

// Секретные поля внутри JSON настроек (или профиля).
// Вычищаются при экспорте без секретов.
void strip_secret_fields(json& j) {
    if (j.contains("server") && j["server"].is_object()) {
        j["server"]["auth_token"] = "";
    }
    // Legacy: ключ openrouter мог остаться в старых профилях
    if (j.contains("openrouter") && j["openrouter"].is_object()) {
        j["openrouter"].erase("api_key");
    }
    // Ключи доступа встроенного llama-server
    if (j.contains("server_runtime") && j["server_runtime"].is_object()) {
        j["server_runtime"]["api_keys"] = json::array();
    }
}

json sanitize_settings_json(json j, bool include_secrets) {
    if (!include_secrets) {
        strip_secret_fields(j);
    }
    return j;
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream file(path);
    if (!file.is_open()) return false;
    std::stringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

} // namespace

bool SettingsSnapshot::export_to_file(const ConfigManager& config,
                                      const std::string& file_path,
                                      bool include_secrets,
                                      std::string& error) {
    const Settings& settings = config.getSettings();
    const std::string profiles_dir = config.getProfilesDirectory();

    json root;
    root["format"] = kSnapshotFormat;
    root["version"] = kSnapshotVersion;
    root["created_utc"] = utc_timestamp_now();
    root["app_version"] = getVersionString();
    root["include_secrets"] = include_secrets;

    std::string active = config.getCurrentProfileName();
    if (active.empty()) {
        active = settings.get_current_profile_name();
    }
    root["active_profile"] = active;

    try {
        // Текущие настройки
        json settings_json = json::parse(settings.serialize_to_json());
        root["settings"] = sanitize_settings_json(std::move(settings_json), include_secrets);

        // Все профили
        json profiles = json::object();
        for (const auto& name : config.listProfiles()) {
            std::string content;
            std::string path = profiles_dir + "/" + name + ".json";
            if (!read_file(path, content)) {
                std::cerr << "[SettingsSnapshot] Не удалось прочитать профиль: " << path << std::endl;
                continue;
            }
            try {
                json profile_json = json::parse(content);
                profiles[name] = sanitize_settings_json(std::move(profile_json), include_secrets);
            } catch (const std::exception& e) {
                std::cerr << "[SettingsSnapshot] Пропущен битый профиль " << name
                          << ": " << e.what() << std::endl;
            }
        }
        root["profiles"] = std::move(profiles);

        // Секреты из .env
        if (include_secrets) {
            json secrets = json::array();
            for (const auto& [key, value] : EnvManager::read_all_keys(profiles_dir)) {
                secrets.push_back({{"key", key}, {"value", value}});
            }
            // Bearer-токен llama-сервера живёт только в settings.ini
            // (в JSON профилей не сериализуется), поэтому переносим отдельно
            const std::string& auth_token = settings.server().auth_token;
            if (!auth_token.empty()) {
                secrets.push_back({{"key", "server.auth_token"}, {"value", auth_token}});
            }
            root["secrets"] = std::move(secrets);
        }

        std::ofstream file(file_path);
        if (!file.is_open()) {
            error = "Не удалось открыть файл для записи: " + file_path;
            return false;
        }
        file << root.dump(2) << "\n";

        std::cout << "[SettingsSnapshot] Экспорт завершён: " << file_path
                  << " (секреты: " << (include_secrets ? "включены" : "исключены") << ")"
                  << std::endl;
        return true;
    } catch (const std::exception& e) {
        error = std::string("Ошибка экспорта: ") + e.what();
        return false;
    }
}

SettingsSnapshot::Info SettingsSnapshot::inspect_file(const std::string& file_path) {
    Info info;
    std::string content;
    if (!read_file(file_path, content)) {
        info.error = "Файл не читается: " + file_path;
        return info;
    }

    try {
        json root = json::parse(content);
        if (!root.is_object() || root.value("format", "") != kSnapshotFormat) {
            info.error = "Это не файл снимка настроек llama-gui";
            return info;
        }
        int version = root.value("version", 0);
        if (version > kSnapshotVersion) {
            info.error = "Версия формата снимка (" + std::to_string(version) +
                         ") новее поддерживаемой (" + std::to_string(kSnapshotVersion) + ")";
            return info;
        }

        info.created_utc = root.value("created_utc", "");
        info.app_version = root.value("app_version", "");
        info.active_profile = root.value("active_profile", "");
        info.include_secrets = root.value("include_secrets", false);

        if (root.contains("profiles") && root["profiles"].is_object()) {
            for (auto it = root["profiles"].begin(); it != root["profiles"].end(); ++it) {
                info.profile_names.push_back(it.key());
            }
        }
        if (root.contains("secrets") && root["secrets"].is_array()) {
            info.secrets_count = root["secrets"].size();
        }
        info.valid = true;
    } catch (const std::exception& e) {
        info.error = std::string("Ошибка разбора файла: ") + e.what();
    }
    return info;
}

int SettingsSnapshot::import_from_file(const std::string& file_path,
                                       ConfigManager& config,
                                       bool include_secrets,
                                       std::string& error) {
    std::string content;
    if (!read_file(file_path, content)) {
        error = "Файл не читается: " + file_path;
        return -1;
    }

    json root;
    try {
        root = json::parse(content);
    } catch (const std::exception& e) {
        error = std::string("Ошибка разбора файла: ") + e.what();
        return -1;
    }

    if (!root.is_object() || root.value("format", "") != kSnapshotFormat) {
        error = "Это не файл снимка настроек llama-gui";
        return -1;
    }

    const std::string profiles_dir = config.getProfilesDirectory();

    try {
        // 1. Записываем профили
        int written = 0;
        bool active_written = false;
        const std::string active_profile = root.value("active_profile", "");

        if (root.contains("profiles") && root["profiles"].is_object()) {
            std::filesystem::create_directories(profiles_dir);
            for (auto it = root["profiles"].begin(); it != root["profiles"].end(); ++it) {
                const std::string& name = it.key();
                if (name.empty() || name == "." || name == ".." ||
                    name.find('/') != std::string::npos ||
                    name.find('\\') != std::string::npos) {
                    std::cerr << "[SettingsSnapshot] Пропущен профиль с недопустимым именем: "
                              << name << std::endl;
                    continue;
                }
                std::ofstream file(profiles_dir + "/" + name + ".json");
                if (!file.is_open()) {
                    error = "Не удалось записать профиль: " + name;
                    return -1;
                }
                file << it.value().dump(4) << "\n";
                ++written;
                if (name == active_profile) {
                    active_written = true;
                }
            }
        }

        // 2. Секреты (по запросу). server.auth_token применяется после
        // активации профиля, поэтому запоминаем его отдельно.
        size_t secrets_applied = 0;
        std::string auth_token;
        if (include_secrets && root.contains("secrets") && root["secrets"].is_array()) {
            for (const auto& entry : root["secrets"]) {
                if (!entry.is_object() || !entry.contains("key")) continue;
                const std::string key = entry.value("key", "");
                if (key == "server.auth_token") {
                    auth_token = entry.value("value", "");
                    ++secrets_applied;
                    continue;
                }
                EnvManager::write_key(key, entry.value("value", ""), profiles_dir);
                ++secrets_applied;
            }
        }

        // 3. Активируем состояние снимка
        Settings& settings = config.getSettings();
        bool activated = false;
        if (active_written && config.loadProfile(active_profile)) {
            activated = true;
        } else if (root.contains("settings")) {
            if (settings.deserialize_from_json(root["settings"].dump())) {
                settings.set_current_profile_name("");
                settings.save_to_ini(settings.get_ini_file_path());
                activated = true;
            } else {
                error = "Не удалось применить настройки из снимка";
                return -1;
            }
        }

        // 4. Bearer-токен сервера (после активации, чтобы не был перезаписан)
        if (!auth_token.empty()) {
            settings.server().auth_token = auth_token;
            settings.save_to_ini(settings.get_ini_file_path());
        }

        std::cout << "[SettingsSnapshot] Импорт завершён: профилей=" << written
                  << ", секретов=" << secrets_applied
                  << ", активирован=" << (activated ? (active_written ? active_profile : "(settings)") : "(нет)")
                  << std::endl;
        return written;
    } catch (const std::exception& e) {
        error = std::string("Ошибка импорта: ") + e.what();
        return -1;
    }
}

} // namespace core
} // namespace llama_gui
