/**
 * @file test_kv_cache_api.cpp
 * @brief Unit тесты для KVCacheAPI
 *
 * Примечание: Эти тесты проверяют базовую функциональность KVCacheAPI
 * без подключения к реальному серверу.
 */

#include "../include/core/kv_cache_types.h"
#include "../include/core/kv_cache_settings.h"
#include "../include/core/kv_cache_metrics.h"
#include <iostream>
#include <cassert>
#include <string>

using namespace llama_gui::core;

// ============================================================================
// Тесты типов
// ============================================================================

void test_slot_status_enum() {
    std::cout << "[TEST] SlotStatus enum... ";

    SlotInfo slot;
    slot.id = 0;
    slot.status = SlotStatus::Idle;

    assert(slot.is_idle() == true);
    assert(slot.is_busy() == false);

    slot.status = SlotStatus::Processing;
    assert(slot.is_idle() == false);
    assert(slot.is_busy() == true);

    slot.status = SlotStatus::Saving;
    assert(slot.is_busy() == true);

    slot.status = SlotStatus::Restoring;
    assert(slot.is_busy() == true);

    std::cout << "PASSED" << std::endl;
}

void test_operation_result() {
    std::cout << "[TEST] KVCacheOperationResult... ";

    KVCacheOperationResult result;

    // Проверка значений по умолчанию
    assert(result.success == false);
    assert(result.slot_id == -1);
    assert(result.n_tokens == 0);
    assert(result.is_success() == false);

    // Устанавливаем успешный результат
    result.success = true;
    result.slot_id = 0;
    result.n_tokens = 1000;
    result.n_bytes = 8000000;
    result.processing_ms = 50.0;

    assert(result.is_success() == true);
    assert(result.n_tokens == 1000);
    assert(result.get_error().empty());

    std::cout << "PASSED" << std::endl;
}

void test_document_info() {
    std::cout << "[TEST] RagDocumentKVInfo... ";

    RagDocumentKVInfo info;
    info.doc_id = "test_doc";
    info.file_path = "/path/to/doc.txt";
    info.file_hash = "abc123";
    info.n_tokens = 5000;
    info.kv_cache_available = true;
    info.kv_cache_path = "/path/to/cache.bin";
    info.kv_cache_size_bytes = 40000000;

    assert(info.has_cache() == true);
    assert(info.is_cache_valid("abc123") == true);
    assert(info.is_cache_valid("xyz789") == false);

    info.kv_cache_available = false;
    assert(info.has_cache() == false);
    assert(info.is_cache_valid("abc123") == false);

    std::cout << "PASSED" << std::endl;
}

// ============================================================================
// Тесты настроек
// ============================================================================

void test_settings_validation() {
    std::cout << "[TEST] KVCacheSettings validation... ";

    KVCacheSettings settings;
    settings.slot_save_path = "/tmp/kv-cache";
    settings.n_parallel = 4;
    settings.cache_type_k = "q8_0";
    settings.cache_type_v = "q8_0";

    assert(settings.validate() == true);
    assert(settings.has_save_path() == true);
    assert(settings.is_quantized() == true);

    // Неверные настройки
    settings.n_parallel = 100;
    assert(settings.validate() == false);

    std::cout << "PASSED" << std::endl;
}

void test_cache_type_validation() {
    std::cout << "[TEST] Cache type validation... ";

    assert(KVCacheSettings::is_valid_cache_type("f32") == true);
    assert(KVCacheSettings::is_valid_cache_type("f16") == true);
    assert(KVCacheSettings::is_valid_cache_type("q8_0") == true);
    assert(KVCacheSettings::is_valid_cache_type("q4_0") == true);
    assert(KVCacheSettings::is_valid_cache_type("q4_1") == true);

    assert(KVCacheSettings::is_valid_cache_type("invalid") == false);
    assert(KVCacheSettings::is_valid_cache_type("") == false);

    auto types = KVCacheSettings::get_valid_cache_types();
    assert(types.size() == 7);

    std::cout << "PASSED" << std::endl;
}

void test_estimate_kv_cache_size() {
    std::cout << "[TEST] KV-cache size estimation... ";

    KVCacheSettings settings;

    settings.cache_type_k = "f16";
    settings.cache_type_v = "f16";
    size_t size_f16 = settings.estimate_kv_cache_size(7.0f, 4096);
    assert(size_f16 > 0);

    settings.cache_type_k = "q8_0";
    settings.cache_type_v = "q8_0";
    size_t size_q8 = settings.estimate_kv_cache_size(7.0f, 4096);
    assert(size_q8 < size_f16);

    settings.cache_type_k = "q4_0";
    settings.cache_type_v = "q4_0";
    size_t size_q4 = settings.estimate_kv_cache_size(7.0f, 4096);
    assert(size_q4 < size_q8);

    size_t size_large_ctx = settings.estimate_kv_cache_size(7.0f, 8192);
    assert(size_large_ctx > size_q4);

    size_t size_70b = settings.estimate_kv_cache_size(70.0f, 4096);
    assert(size_70b > size_q4);

    std::cout << "PASSED" << std::endl;
}

// ============================================================================
// Тесты метрик
// ============================================================================

void test_metrics_manager() {
    std::cout << "[TEST] KVCacheMetricsManager... ";

    auto metrics_mgr = std::make_unique<KVCacheMetricsManager>();

    metrics_mgr->record_save(1000, 8000000, 50.0);
    metrics_mgr->record_save(2000, 16000000, 100.0);
    metrics_mgr->record_restore(1000, 8000000, 10.0);
    metrics_mgr->record_restore(1000, 8000000, 8.0);
    metrics_mgr->record_cache_hit();
    metrics_mgr->record_cache_hit();
    metrics_mgr->record_cache_miss();

    auto metrics = metrics_mgr->get_metrics();

    assert(metrics.total_saves == 2);
    assert(metrics.total_restores == 2);
    assert(metrics.cache_hits == 2);
    assert(metrics.cache_misses == 1);
    assert(metrics.hit_rate_percent() == 66);

    std::string json_str = metrics_mgr->to_json();
    assert(!json_str.empty());
    assert(json_str.find("total_saves") != std::string::npos);

    std::string report = metrics_mgr->generate_report();
    assert(!report.empty());
    assert(report.find("KV-Cache Metrics Report") != std::string::npos);

    int score = metrics_mgr->get_efficiency_score();
    assert(score >= 0 && score <= 100);

    std::cout << "PASSED" << std::endl;
}

void test_metrics_reset() {
    std::cout << "[TEST] Metrics reset... ";

    auto metrics_mgr = std::make_unique<KVCacheMetricsManager>();

    metrics_mgr->record_save(1000, 8000000, 50.0);
    metrics_mgr->record_restore(1000, 8000000, 10.0);

    metrics_mgr->reset();

    auto metrics = metrics_mgr->get_metrics();
    assert(metrics.total_saves == 0);
    assert(metrics.total_restores == 0);

    std::cout << "PASSED" << std::endl;
}

void test_prompt_caching_metrics() {
    std::cout << "[TEST] Prompt caching metrics... ";

    auto metrics_mgr = std::make_unique<KVCacheMetricsManager>();

    // Записать несколько кэшированных промптов
    metrics_mgr->record_cached_prompt(5.0);
    metrics_mgr->record_cached_prompt(4.5);
    metrics_mgr->record_cached_prompt(5.5);

    // Записать несколько некэшированных промптов
    metrics_mgr->record_uncached_prompt(50.0);
    metrics_mgr->record_uncached_prompt(48.0);
    metrics_mgr->record_uncached_prompt(52.0);

    auto metrics = metrics_mgr->get_metrics();

    assert(metrics.cached_prompt_queries == 3);
    assert(metrics.uncached_prompt_queries == 3);
    assert(metrics.prompt_cache_rate_percent() == 50);

    // Проверить speedup (некэшированные должны быть медленнее)
    float speedup = metrics.prompt_speedup();
    assert(speedup > 1.0f);  // Кэшированные должны быть быстрее

    // Проверить JSON
    std::string json_str = metrics_mgr->to_json();
    assert(json_str.find("cached_prompt_queries") != std::string::npos);
    assert(json_str.find("prompt_speedup") != std::string::npos);

    // Проверить отчёт
    std::string report = metrics_mgr->generate_report();
    assert(report.find("Prompt Caching") != std::string::npos);
    assert(report.find("Prompt speedup") != std::string::npos);

    std::cout << "PASSED" << std::endl;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    std::cout << "========================================" << std::endl;
    std::cout << "KV-Cache API Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    try {
        test_slot_status_enum();
        test_operation_result();
        test_document_info();
        test_settings_validation();
        test_cache_type_validation();
        test_estimate_kv_cache_size();
        test_metrics_manager();
        test_metrics_reset();
        test_prompt_caching_metrics();

        std::cout << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "All tests PASSED!" << std::endl;
        std::cout << "========================================" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test FAILED with exception: " << e.what() << std::endl;
        return 1;
    }
}
