#include "../include/core/kv_cache_metrics.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace llama_gui {
namespace core {

using json = nlohmann::json;

// ============================================================================
// Constructor/Destructor
// ============================================================================

KVCacheMetricsManager::KVCacheMetricsManager() : metrics_() {}

KVCacheMetricsManager::~KVCacheMetricsManager() = default;

// ============================================================================
// Запись метрик
// ============================================================================

void KVCacheMetricsManager::record_save(size_t tokens, size_t bytes, double time_ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    metrics_.total_saves++;
    metrics_.total_bytes_written += bytes;

    // Скользящее среднее для времени сохранения
    double alpha = 0.1;
    metrics_.avg_save_time_ms = alpha * time_ms + (1.0 - alpha) * metrics_.avg_save_time_ms;
}

void KVCacheMetricsManager::record_restore(size_t tokens, size_t bytes, double time_ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    metrics_.total_restores++;
    metrics_.total_bytes_read += bytes;

    // Скользящее среднее для времени восстановления
    double alpha = 0.1;
    metrics_.avg_restore_time_ms = alpha * time_ms + (1.0 - alpha) * metrics_.avg_restore_time_ms;
}

void KVCacheMetricsManager::record_cache_hit() {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_.cache_hits++;
}

void KVCacheMetricsManager::record_cache_miss() {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_.cache_misses++;
}

void KVCacheMetricsManager::record_cached_prompt(double time_ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    metrics_.cached_prompt_queries++;

    // Скользящее среднее для времени обработки кэшированного промпта
    double alpha = 0.1;
    metrics_.avg_cached_prompt_time_ms = alpha * time_ms + (1.0 - alpha) * metrics_.avg_cached_prompt_time_ms;
}

void KVCacheMetricsManager::record_uncached_prompt(double time_ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    metrics_.uncached_prompt_queries++;

    // Скользящее среднее для времени обработки некэшированного промпта
    double alpha = 0.1;
    metrics_.avg_uncached_prompt_time_ms = alpha * time_ms + (1.0 - alpha) * metrics_.avg_uncached_prompt_time_ms;
}

// ============================================================================
// Чтение метрик
// ============================================================================

KVCacheMetrics KVCacheMetricsManager::get_metrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metrics_;
}

std::string KVCacheMetricsManager::to_json() const {
    std::lock_guard<std::mutex> lock(mutex_);

    json j;
    j["total_saves"] = metrics_.total_saves;
    j["total_restores"] = metrics_.total_restores;
    j["total_bytes_written"] = metrics_.total_bytes_written;
    j["total_bytes_read"] = metrics_.total_bytes_read;
    j["avg_save_time_ms"] = std::round(metrics_.avg_save_time_ms * 100.0) / 100.0;
    j["avg_restore_time_ms"] = std::round(metrics_.avg_restore_time_ms * 100.0) / 100.0;
    j["cache_hits"] = metrics_.cache_hits;
    j["cache_misses"] = metrics_.cache_misses;
    j["hit_rate_percent"] = metrics_.hit_rate_percent();

    // Prompt caching метрики
    j["cached_prompt_queries"] = metrics_.cached_prompt_queries;
    j["uncached_prompt_queries"] = metrics_.uncached_prompt_queries;
    j["avg_cached_prompt_time_ms"] = std::round(metrics_.avg_cached_prompt_time_ms * 100.0) / 100.0;
    j["avg_uncached_prompt_time_ms"] = std::round(metrics_.avg_uncached_prompt_time_ms * 100.0) / 100.0;
    j["prompt_cache_rate_percent"] = metrics_.prompt_cache_rate_percent();
    j["prompt_speedup"] = std::round(metrics_.prompt_speedup() * 100.0) / 100.0;

    return j.dump(2);
}

void KVCacheMetricsManager::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_.reset();
}

// ============================================================================
// Отчёты
// ============================================================================

std::string KVCacheMetricsManager::generate_report() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::stringstream ss;
    ss << "=== KV-Cache Metrics Report ===\n";
    ss << "\n";

    // Операции
    ss << "Operations:\n";
    ss << "  Total saves: " << metrics_.total_saves << "\n";
    ss << "  Total restores: " << metrics_.total_restores << "\n";
    ss << "\n";

    // Данные
    ss << "Data Transfer:\n";
    ss << "  Total bytes written: " << metrics_.total_bytes_written
       << " (" << std::round(metrics_.total_bytes_written / 1024.0 / 1024.0 * 100.0) / 100.0 << " MB)\n";
    ss << "  Total bytes read: " << metrics_.total_bytes_read
       << " (" << std::round(metrics_.total_bytes_read / 1024.0 / 1024.0 * 100.0) / 100.0 << " MB)\n";
    ss << "\n";

    // Время
    ss << "Timing:\n";
    ss << "  Avg save time: " << std::round(metrics_.avg_save_time_ms * 100.0) / 100.0 << " ms\n";
    ss << "  Avg restore time: " << std::round(metrics_.avg_restore_time_ms * 100.0) / 100.0 << " ms\n";
    ss << "\n";

    // Cache efficiency
    ss << "Cache Efficiency:\n";
    ss << "  Cache hits: " << metrics_.cache_hits << "\n";
    ss << "  Cache misses: " << metrics_.cache_misses << "\n";
    ss << "  Hit rate: " << metrics_.hit_rate_percent() << "%\n";
    ss << "\n";

    // Prompt caching метрики
    ss << "Prompt Caching:\n";
    ss << "  Cached prompt queries: " << metrics_.cached_prompt_queries << "\n";
    ss << "  Uncached prompt queries: " << metrics_.uncached_prompt_queries << "\n";
    ss << "  Prompt cache rate: " << metrics_.prompt_cache_rate_percent() << "%\n";
    ss << "  Avg cached prompt time: " << std::round(metrics_.avg_cached_prompt_time_ms * 100.0) / 100.0 << " ms\n";
    ss << "  Avg uncached prompt time: " << std::round(metrics_.avg_uncached_prompt_time_ms * 100.0) / 100.0 << " ms\n";
    ss << "  Prompt speedup: " << std::round(metrics_.prompt_speedup() * 100.0) / 100.0 << "x\n";
    ss << "\n";

    // Efficiency score
    ss << "Efficiency Score: " << get_efficiency_score() << "/100\n";

    return ss.str();
}

int KVCacheMetricsManager::get_efficiency_score() const {
    // Вычисляем score на основе:
    // - Hit rate (50% веса)
    // - Соотношение save/restore (25% веса)
    // - Эффективность сжатия (25% веса)

    int score = 0;

    // Hit rate score (0-50)
    float hit_rate = metrics_.hit_rate();
    score += static_cast<int>(hit_rate * 50.0f);

    // Save/restore balance score (0-25)
    // Если restore >= save, это хорошо (переиспользование кэша)
    if (metrics_.total_saves > 0) {
        float restore_ratio = static_cast<float>(metrics_.total_restores) /
                              static_cast<float>(metrics_.total_saves);
        if (restore_ratio >= 1.0f) {
            score += 25;  // Отличное переиспользование
        } else if (restore_ratio >= 0.5f) {
            score += 15;  // Хорошее переиспользование
        } else if (restore_ratio >= 0.25f) {
            score += 8;   // Среднее переиспользование
        } else {
            score += 3;   // Плохое переиспользование
        }
    }

    // Compression efficiency score (0-25)
    // Сравниваем среднее время save vs restore
    // Если restore быстрее save, это хорошо
    if (metrics_.avg_save_time_ms > 0 && metrics_.avg_restore_time_ms > 0) {
        float speedup = static_cast<float>(metrics_.avg_save_time_ms) /
                        static_cast<float>(metrics_.avg_restore_time_ms);
        if (speedup >= 5.0f) {
            score += 25;  // Отличное ускорение (5x+)
        } else if (speedup >= 2.0f) {
            score += 18;  // Хорошее ускорение (2x+)
        } else if (speedup >= 1.0f) {
            score += 10;  // Небольшое ускорение
        } else {
            score += 5;   // Restore медленнее save
        }
    } else if (metrics_.avg_restore_time_ms > 0) {
        score += 10;  // Есть restore, но нет save для сравнения
    }

    return score;
}

} // namespace core
} // namespace llama_gui
