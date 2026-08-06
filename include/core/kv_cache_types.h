#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <ctime>

namespace llama_gui {
namespace core {

/**
 * @brief Статус слота KV-cache
 */
enum class SlotStatus {
    Idle,           // Слот свободен
    Processing,     // Слот обрабатывает запрос
    Saving,         // Сохранение KV-cache
    Restoring,      // Восстановление KV-cache
    Error           // Ошибка слота
};

/**
 * @brief Информация о слоте
 */
struct SlotInfo {
    int id = -1;
    SlotStatus status = SlotStatus::Idle;
    size_t num_prompt_tokens = 0;
    size_t num_infered_tokens = 0;
    time_t last_activity = 0;
    std::string loaded_document_id;  // ID загруженного документа

    /**
     * @brief Проверка доступности слота
     * @return true если слот свободен
     */
    bool is_idle() const {
        return status == SlotStatus::Idle;
    }

    /**
     * @brief Проверка занятости слота
     * @return true если слот занят
     */
    bool is_busy() const {
        return status == SlotStatus::Processing ||
               status == SlotStatus::Saving ||
               status == SlotStatus::Restoring;
    }
};

/**
 * @brief Результат операции с KV-cache
 */
struct KVCacheOperationResult {
    bool success = false;
    int slot_id = -1;
    std::string filename;
    size_t n_tokens = 0;          // Количество токенов
    size_t n_bytes = 0;           // Размер файла в байтах
    double processing_ms = 0.0;   // Время операции в мс
    std::string error_message;

    /**
     * @brief Проверка успешности операции
     * @return true если операция успешна
     */
    bool is_success() const {
        return success;
    }

    /**
     * @brief Получить описание ошибки
     * @return Строка с описанием ошибки
     */
    std::string get_error() const {
        return error_message;
    }
};

/**
 * @brief Информация о документе в RAG с KV-cache
 */
struct RagDocumentKVInfo {
    std::string doc_id;
    std::string file_path;
    std::string file_hash;        // Hash для проверки изменений
    size_t n_tokens = 0;          // Количество токенов
    time_t last_processed = 0;    // Время обработки
    bool kv_cache_available = false;  // Есть ли KV-cache
    std::string kv_cache_path;    // Путь к файлу
    size_t kv_cache_size_bytes = 0;   // Размер файла KV-cache

    /**
     * @brief Проверка наличия KV-cache
     * @return true если KV-cache доступен
     */
    bool has_cache() const {
        return kv_cache_available;
    }

    /**
     * @brief Проверка актуальности KV-cache
     * @param current_hash Текущий hash файла
     * @return true если hash совпадает
     */
    bool is_cache_valid(const std::string& current_hash) const {
        return kv_cache_available && file_hash == current_hash;
    }
};

/**
 * @brief Метрики KV-cache
 */
struct KVCacheMetrics {
    size_t total_saves = 0;
    size_t total_restores = 0;
    size_t total_bytes_written = 0;
    size_t total_bytes_read = 0;
    double avg_save_time_ms = 0.0;
    double avg_restore_time_ms = 0.0;
    size_t cache_hits = 0;
    size_t cache_misses = 0;

    // Prompt caching метрики
    size_t cached_prompt_queries = 0;       // Количество запросов с кэшированным промптом
    size_t uncached_prompt_queries = 0;     // Количество запросов без кэшированного промпта
    double avg_cached_prompt_time_ms = 0.0; // Среднее время обработки кэшированного промпта
    double avg_uncached_prompt_time_ms = 0.0; // Среднее время обработки некэшированного промпта

    /**
     * @brief Получить общий процент попаданий в кэш
     * @return Hit rate от 0.0 до 1.0
     */
    float hit_rate() const {
        size_t total = cache_hits + cache_misses;
        return total > 0 ? static_cast<float>(cache_hits) / total : 0.0f;
    }

    /**
     * @brief Получить общий процент попаданий в процентах
     * @return Hit rate от 0 до 100
     */
    int hit_rate_percent() const {
        return static_cast<int>(hit_rate() * 100.0f);
    }

    /**
     * @brief Получить процент кэшированных промптов
     * @return Prompt cache rate от 0.0 до 1.0
     */
    float prompt_cache_rate() const {
        size_t total = cached_prompt_queries + uncached_prompt_queries;
        return total > 0 ? static_cast<float>(cached_prompt_queries) / total : 0.0f;
    }

    /**
     * @brief Получить процент кэшированных промптов в процентах
     * @return Prompt cache rate от 0 до 100
     */
    int prompt_cache_rate_percent() const {
        return static_cast<int>(prompt_cache_rate() * 100.0f);
    }

    /**
     * @brief Получить ускорение благодаря кэшированию промптов
     * @return Коэффициент ускорения (1.0 = нет ускорения, >1.0 = есть ускорение)
     */
    float prompt_speedup() const {
        if (avg_cached_prompt_time_ms <= 0 || avg_uncached_prompt_time_ms <= 0) {
            return 1.0f;
        }
        return avg_uncached_prompt_time_ms / avg_cached_prompt_time_ms;
    }

    /**
     * @brief Сбросить все метрики
     */
    void reset() {
        *this = KVCacheMetrics{};
    }
};

} // namespace core
} // namespace llama_gui
