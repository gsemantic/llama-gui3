/**
 * @file test_kv_cache_integration.cpp
 * @brief Интеграционные тесты для KV-cache модуля
 *
 * Тестируют взаимодействие между компонентами KV-cache:
 * - Storage
 * - SlotManager
 * - Metrics
 * - Settings
 */

#include "../include/core/kv_cache_storage.h"
#include "../include/core/kv_cache_metrics.h"
#include "../include/core/kv_cache_settings.h"
#include "../include/core/kv_cache_types.h"
#include <iostream>
#include <cassert>
#include <memory>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace llama_gui::core;

// ============================================================================
// Тесты
// ============================================================================

std::string get_test_cache_path() {
    return "/tmp/llama-gui-kv-cache-integration-test";
}

void cleanup_test_cache() {
    try {
        if (fs::exists(get_test_cache_path())) {
            fs::remove_all(get_test_cache_path());
        }
    } catch (...) {}
}

void test_settings_with_storage() {
    std::cout << "[INTEGRATION] Testing settings with storage... ";
    cleanup_test_cache();

    KVCacheSettings settings;
    settings.slot_save_path = get_test_cache_path();
    settings.n_parallel = 4;
    settings.cache_type_k = "q8_0";
    settings.cache_type_v = "q8_0";

    assert(settings.validate() == true);

    // Создаём хранилище с путём из настроек
    auto storage = std::make_unique<KVCacheStorage>(settings.slot_save_path);
    assert(storage->get_base_path() == settings.slot_save_path);
    assert(storage->is_writable() == true);

    cleanup_test_cache();
    std::cout << "PASSED" << std::endl;
}

void test_storage_with_doc_ids() {
    std::cout << "[INTEGRATION] Testing storage with document IDs... ";
    cleanup_test_cache();

    auto storage = std::make_unique<KVCacheStorage>(get_test_cache_path());

    std::vector<std::string> doc_ids = {"doc_001", "doc_002", "doc_003"};

    for (const auto& doc_id : doc_ids) {
        std::string path = storage->get_cache_path(doc_id);
        std::ofstream file(path, std::ios::binary);
        file << "KV-cache data for " << doc_id;
        file.close();
    }

    for (const auto& doc_id : doc_ids) {
        assert(storage->has_cache(doc_id) == true);
        assert(storage->get_cache_size(doc_id) > 0);
    }

    auto cached_docs = storage->get_all_cached_documents();
    assert(cached_docs.size() == 3);

    cleanup_test_cache();
    std::cout << "PASSED" << std::endl;
}

void test_metrics_with_settings() {
    std::cout << "[INTEGRATION] Testing metrics with settings... ";

    KVCacheSettings settings;
    settings.cache_type_k = "q8_0";
    settings.cache_type_v = "q8_0";

    auto metrics_mgr = std::make_unique<KVCacheMetricsManager>();

    // Симулируем операции с размерами на основе настроек
    size_t estimated_size = settings.estimate_kv_cache_size(7.0f, 4096);
    metrics_mgr->record_save(1000, estimated_size, 50.0);
    metrics_mgr->record_restore(1000, estimated_size, 10.0);

    auto metrics = metrics_mgr->get_metrics();
    assert(metrics.total_saves == 1);
    assert(metrics.total_restores == 1);
    assert(metrics.total_bytes_written >= estimated_size);

    std::string report = metrics_mgr->generate_report();
    assert(report.find("KV-Cache Metrics Report") != std::string::npos);

    std::cout << "PASSED" << std::endl;
}

void test_cache_type_efficiency() {
    std::cout << "[INTEGRATION] Testing cache type efficiency... ";

    KVCacheSettings settings_f16, settings_q8, settings_q4;
    
    settings_f16.cache_type_k = "f16";
    settings_f16.cache_type_v = "f16";
    
    settings_q8.cache_type_k = "q8_0";
    settings_q8.cache_type_v = "q8_0";
    
    settings_q4.cache_type_k = "q4_0";
    settings_q4.cache_type_v = "q4_0";

    size_t size_f16 = settings_f16.estimate_kv_cache_size(7.0f, 4096);
    size_t size_q8 = settings_q8.estimate_kv_cache_size(7.0f, 4096);
    size_t size_q4 = settings_q4.estimate_kv_cache_size(7.0f, 4096);

    // Проверяем что квантование уменьшает размер
    assert(size_f16 > size_q8);
    assert(size_q8 > size_q4);

    // Проверяем коэффициенты сжатия
    float q8_ratio = static_cast<float>(size_q8) / static_cast<float>(size_f16);
    float q4_ratio = static_cast<float>(size_q4) / static_cast<float>(size_f16);

    assert(q8_ratio < 0.6f);  // q8_0 должен быть меньше 50%
    assert(q4_ratio < 0.3f);  // q4_0 должен быть меньше 30%

    std::cout << "PASSED" << std::endl;
}

void test_storage_cleanup() {
    std::cout << "[INTEGRATION] Testing storage cleanup... ";
    cleanup_test_cache();

    auto storage = std::make_unique<KVCacheStorage>(get_test_cache_path());

    // Создаём файлы
    for (int i = 0; i < 5; i++) {
        std::string doc_id = "doc_" + std::to_string(i);
        std::string path = storage->get_cache_path(doc_id);
        std::ofstream file(path);
        file << "data " << i;
        file.close();
    }

    assert(storage->get_file_count() == 5);

    // Очищаем все
    int deleted = storage->cleanup_old_caches(0);
    assert(deleted == 5);
    assert(storage->get_file_count() == 0);

    cleanup_test_cache();
    std::cout << "PASSED" << std::endl;
}

void test_metrics_efficiency_score() {
    std::cout << "[INTEGRATION] Testing metrics efficiency score... ";

    auto metrics_mgr = std::make_unique<KVCacheMetricsManager>();

    // Симулируем хорошее переиспользование кэша
    for (int i = 0; i < 10; i++) {
        metrics_mgr->record_save(1000, 8000000, 50.0);
    }
    for (int i = 0; i < 20; i++) {
        metrics_mgr->record_restore(1000, 8000000, 5.0);
    }
    for (int i = 0; i < 15; i++) {
        metrics_mgr->record_cache_hit();
    }
    for (int i = 0; i < 5; i++) {
        metrics_mgr->record_cache_miss();
    }

    auto metrics = metrics_mgr->get_metrics();
    int score = metrics_mgr->get_efficiency_score();

    // Должен быть высокий score из-за хорошего hit rate и restore/save ratio
    assert(score > 50);
    assert(metrics.hit_rate_percent() == 75);  // 15/20 = 75%

    std::cout << "PASSED" << std::endl;
}

void test_serialization_roundtrip() {
    std::cout << "[INTEGRATION] Testing serialization roundtrip... ";

    KVCacheSettings original;
    original.slot_save_path = "/test/path";
    original.n_parallel = 8;
    original.cache_type_k = "q4_0";
    original.cache_type_v = "q4_1";
    original.cache_reuse = 256;
    original.max_storage_size_mb = 5120;
    original.max_file_age_seconds = 86400;

    // Сериализация
    nlohmann::json j = original;

    // Десериализация
    KVCacheSettings restored = j;

    assert(restored.slot_save_path == original.slot_save_path);
    assert(restored.n_parallel == original.n_parallel);
    assert(restored.cache_type_k == original.cache_type_k);
    assert(restored.cache_type_v == original.cache_type_v);
    assert(restored.cache_reuse == original.cache_reuse);
    assert(restored.max_storage_size_mb == original.max_storage_size_mb);
    assert(restored.max_file_age_seconds == original.max_file_age_seconds);

    // Проверяем что восстановленные настройки валидны
    assert(restored.validate() == true);

    std::cout << "PASSED" << std::endl;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    std::cout << "========================================" << std::endl;
    std::cout << "KV-Cache Integration Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    try {
        test_settings_with_storage();
        test_storage_with_doc_ids();
        test_metrics_with_settings();
        test_cache_type_efficiency();
        test_storage_cleanup();
        test_metrics_efficiency_score();
        test_serialization_roundtrip();

        std::cout << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "All integration tests PASSED!" << std::endl;
        std::cout << "========================================" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test FAILED with exception: " << e.what() << std::endl;
        return 1;
    }
}
