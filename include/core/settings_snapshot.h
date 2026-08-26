#pragma once

#include <string>
#include <vector>

namespace llama_gui {
namespace core {

class ConfigManager;

struct SnapshotSecretEntry {
    std::string key;
    std::string value;
};

/**
 * @class SettingsSnapshot
 * @brief Переносимый снимок настроек в едином JSON-файле
 *
 * Снимок содержит:
 * - текущие настройки (те же данные, что в профиле)
 * - все профили из profiles/*.json
 * - опционально секреты: profiles/.env (API-ключи) + server.auth_token
 *
 * Формат файла:
 * {
 *   "format": "llama-gui-settings-snapshot",
 *   "version": 1,
 *   "created_utc": "...",
 *   "app_version": "...",
 *   "active_profile": "default",
 *   "include_secrets": true,
 *   "settings": { ... },
 *   "profiles": { "name": { ... }, ... },
 *   "secrets": [ {"key": "...", "value": "..."} ]
 * }
 *
 * Если include_secrets == false, секретные поля вычищаются из settings
 * и всех профилей, а секция secrets не записывается. Такой файл безопасно
 * передавать между машинами/пользователями.
 */
class SettingsSnapshot {
public:
    /// Метаданные снимка для предпросмотра перед импортом
    struct Info {
        bool valid = false;
        std::string error;
        std::string created_utc;
        std::string app_version;
        std::string active_profile;
        bool include_secrets = false;
        std::vector<std::string> profile_names;
        size_t secrets_count = 0;
    };

    /**
     * @brief Экспортировать снимок настроек в файл
     * @param config менеджер конфигурации (источник данных)
     * @param file_path путь к создаваемому файлу
     * @param include_secrets включить ли секреты (.env, auth_token)
     * @param error описание ошибки при неудаче
     */
    static bool export_to_file(const ConfigManager& config,
                               const std::string& file_path,
                               bool include_secrets,
                               std::string& error);

    /**
     * @brief Прочитать из файла только метаданные (без применения)
     */
    static Info inspect_file(const std::string& file_path);

    /**
     * @brief Импортировать снимок из файла
     *
     * Записывает профили в profiles/, при include_secrets переносит секреты
     * в .env, затем активирует профиль-снимок (или секцию settings, если
     * активный профиль не найден) и синхронизирует settings.ini.
     *
     * @param file_path путь к файлу снимка
     * @param config менеджер конфигурации (приёмник)
     * @param include_secrets переносить ли секреты из файла (если они там есть)
     * @param error описание ошибки при неудаче
     * @return количество импортированных профилей, -1 при ошибке
     */
    static int import_from_file(const std::string& file_path,
                                ConfigManager& config,
                                bool include_secrets,
                                std::string& error);
};

} // namespace core
} // namespace llama_gui
