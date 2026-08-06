#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace llama_gui {
namespace core {

/**
 * @brief Настройки KV-cache модуля
 *
 * Параметры конфигурации:
 * - Путь сохранения
 * - Типы квантования K/V cache
 * - Настройки переиспользования
 * - Лимиты хранилища
 */
struct KVCacheSettings {
    // =========================================================================
    // Основные настройки
    // =========================================================================

    /// Путь для сохранения KV-cache слотов
    std::string slot_save_path = "";

    /// Количество параллельных слотов
    int n_parallel = 4;

    // =========================================================================
    // Типы данных KV-cache
    // =========================================================================

    /// Тип K cache (f32/f16/bf16/q8_0/q4_0/q4_1/iq4_nl)
    std::string cache_type_k = "q8_0";

    /// Тип V cache (f32/f16/bf16/q8_0/q4_0/q4_1/iq4_nl)
    std::string cache_type_v = "q8_0";

    // =========================================================================
    // Переиспользование
    // =========================================================================

    /// Минимальный размер чанка для reuse (0 = отключено)
    int cache_reuse = 0;

    /// Включить автоматическое переиспользование
    bool auto_reuse_enabled = true;

    // =========================================================================
    // Лимиты хранилища
    // =========================================================================

    /// Максимальный размер хранилища в МБ (0 = без ограничений)
    size_t max_storage_size_mb = 10240;  // 10GB по умолчанию

    /// Максимальный возраст файла в секундах (0 = без ограничений)
    int max_file_age_seconds = 604800;  // 7 дней

    /// Автоматическая очистка при превышении лимита
    bool auto_cleanup_enabled = true;

    // =========================================================================
    // Методы валидации
    // =========================================================================

    /**
     * @brief Проверка корректности настроек
     * @return true если все параметры валидны
     */
    bool validate() const;

    /**
     * @brief Получить строку с ошибками валидации
     * @return Описание ошибок
     */
    std::string get_validation_errors() const;

    /**
     * @brief Проверка наличия пути сохранения
     * @return true если путь задан
     */
    bool has_save_path() const {
        return !slot_save_path.empty();
    }

    /**
     * @brief Проверка включённого квантования
     * @return true если используется квантование
     */
    bool is_quantized() const {
        return cache_type_k != "f32" && cache_type_k != "f16" &&
               cache_type_v != "f32" && cache_type_v != "f16";
    }

    /**
     * @brief Проверка допустимого типа кэша
     * @param type Строка типа кэша
     * @return true если тип допустим
     */
    static bool is_valid_cache_type(const std::string& type);

    /**
     * @brief Получить список допустимых типов кэша
     * @return Вектор допустимых типов
     */
    static std::vector<std::string> get_valid_cache_types();

    /**
     * @brief Оценить размер KV-cache на токен в байтах
     * @param model_params_b Количество параметров модели (в миллиардах)
     * @return Размер на токен
     */
    size_t estimate_bytes_per_token(float model_params_b) const;

    /**
     * @brief Получить размер KV-cache для модели
     * @param model_params_b Количество параметров модели (в миллиардах)
     * @param ctx_size Размер контекста
     * @return Размер в байтах
     */
    size_t estimate_kv_cache_size(float model_params_b, int ctx_size) const;
};

/**
 * @brief Сериализация в JSON
 * @param j JSON объект
 * @param s Настройки
 */
void to_json(nlohmann::json& j, const KVCacheSettings& s);

/**
 * @brief Десериализация из JSON
 * @param j JSON объект
 * @param s Настройки
 */
void from_json(const nlohmann::json& j, KVCacheSettings& s);

} // namespace core
} // namespace llama_gui
