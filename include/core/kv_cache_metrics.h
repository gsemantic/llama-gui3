#pragma once

#include "kv_cache_types.h"
#include <string>
#include <memory>
#include <mutex>

namespace llama_gui {
namespace core {

/**
 * @brief Менеджер метрик KV-cache
 *
 * Собирает и агрегирует метрики:
 * - Количество операций save/restore
 * - Объём переданных данных
 * - Время операций
 * - Cache hit/miss rate
 */
class KVCacheMetricsManager {
public:
    /**
     * @brief Конструктор менеджера метрик
     */
    KVCacheMetricsManager();

    /**
     * @brief Деструктор
     */
    ~KVCacheMetricsManager();

    // =========================================================================
    // Запись метрик
    // =========================================================================

    /**
     * @brief Записать операцию сохранения
     * @param tokens Количество токенов
     * @param bytes Размер данных
     * @param time_ms Время операции
     */
    void record_save(size_t tokens, size_t bytes, double time_ms);

    /**
     * @brief Записать операцию восстановления
     * @param tokens Количество токенов
     * @param bytes Размер данных
     * @param time_ms Время операции
     */
    void record_restore(size_t tokens, size_t bytes, double time_ms);

    /**
     * @brief Записать cache hit
     */
    void record_cache_hit();

    /**
     * @brief Записать cache miss
     */
    void record_cache_miss();

    /**
     * @brief Записать обработку кэшированного промпта
     * @param time_ms Время обработки промпта
     */
    void record_cached_prompt(double time_ms);

    /**
     * @brief Записать обработку некэшированного промпта
     * @param time_ms Время обработки промпта
     */
    void record_uncached_prompt(double time_ms);

    // =========================================================================
    // Чтение метрик
    // =========================================================================

    /**
     * @brief Получить текущие метрики
     * @return Копия метрик
     */
    KVCacheMetrics get_metrics() const;

    /**
     * @brief Получить метрики в формате JSON
     * @return JSON строка
     */
    std::string to_json() const;

    /**
     * @brief Сбросить все метрики
     */
    void reset();

    // =========================================================================
    // Отчёты
    // =========================================================================

    /**
     * @brief Сгенерировать текстовый отчёт
     * @return Форматированный отчёт
     */
    std::string generate_report() const;

    /**
     * @brief Получить efficiency score (0-100)
     * @return Оценка эффективности
     */
    int get_efficiency_score() const;

private:
    KVCacheMetrics metrics_;
    mutable std::mutex mutex_;
};

} // namespace core
} // namespace llama_gui
