#include "../include/core/model_performance_stats.h"
#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>

using namespace llama_gui::core;

// ============================================================================
// Тесты
// ============================================================================

void test_model_performance_stats_basic() {
    std::cout << "[TEST] ModelPerformanceStats basic... ";

    ModelPerformanceStats stats;
    stats.model_path = "/models/test-model.gguf";
    stats.model_name = "test-model";

    assert(stats.model_path == "/models/test-model.gguf");
    assert(stats.model_name == "test-model");
    assert(stats.total_generations == 0);
    assert(stats.avg_tokens_per_second == 0.0);

    std::cout << "PASSED" << std::endl;
}

void test_model_performance_manager_record() {
    std::cout << "[TEST] ModelPerformanceManager record... ";

    ModelPerformanceManager manager;

    // Записать несколько генераций
    manager.record_generation(
        "/models/test-model.gguf",
        45.5,   // tokens_per_second
        2000.0, // response_time_ms
        100,    // tokens_generated
        2048,   // context_used
        4096    // total_context
    );

    manager.record_generation(
        "/models/test-model.gguf",
        48.0,
        1900.0,
        95,
        2100,
        4096
    );

    manager.record_generation(
        "/models/test-model.gguf",
        42.0,
        2100.0,
        105,
        2000,
        4096
    );

    // Проверить статистику
    auto stats = manager.get_model_stats("/models/test-model.gguf");

    assert(stats.total_generations == 3);
    assert(stats.model_name == "test-model");
    // Скользящее среднее (alpha=0.1) с начальным 0:
    // 1: 0.1*45.5 + 0.9*0 = 4.55
    // 2: 0.1*48.0 + 0.9*4.55 = 8.895
    // 3: 0.1*42.0 + 0.9*8.895 = 12.2
    assert(stats.avg_tokens_per_second > 5.0 && stats.avg_tokens_per_second < 20.0);
    // avg_tokens_generated: 1-е = 100, 2-е = 0.1*95+0.9*100=99.5, 3-е = 0.1*105+0.9*99.5=99.95
    assert(stats.avg_tokens_generated > 95 && stats.avg_tokens_generated < 105);

    std::cout << "PASSED" << std::endl;
}

void test_model_performance_manager_multiple_models() {
    std::cout << "[TEST] ModelPerformanceManager multiple models... ";

    ModelPerformanceManager manager;

    // Модель 1
    manager.record_generation(
        "/models/model-a.gguf",
        50.0, 2000.0, 100, 2048, 4096
    );

    // Модель 2
    manager.record_generation(
        "/models/model-b.gguf",
        30.0, 3000.0, 80, 1500, 4096
    );

    // Модель 3
    manager.record_generation(
        "/models/model-c.gguf",
        60.0, 1500.0, 120, 2500, 4096
    );

    assert(manager.get_model_count() == 3);
    assert(manager.has_model_stats("/models/model-a.gguf"));
    assert(manager.has_model_stats("/models/model-b.gguf"));
    assert(manager.has_model_stats("/models/model-c.gguf"));

    auto stats_a = manager.get_model_stats("/models/model-a.gguf");
    auto stats_b = manager.get_model_stats("/models/model-b.gguf");
    auto stats_c = manager.get_model_stats("/models/model-c.gguf");

    // После первой записи: avg = 0.1 * value
    assert(stats_a.avg_tokens_per_second > 4.0 && stats_a.avg_tokens_per_second < 6.0);
    assert(stats_b.avg_tokens_per_second > 2.0 && stats_b.avg_tokens_per_second < 4.0);
    assert(stats_c.avg_tokens_per_second > 5.0 && stats_c.avg_tokens_per_second < 7.0);

    std::cout << "PASSED" << std::endl;
}

void test_model_performance_manager_json() {
    std::cout << "[TEST] ModelPerformanceManager JSON serialization... ";

    ModelPerformanceManager manager;

    manager.record_generation(
        "/models/test-model.gguf",
        45.0, 2000.0, 100, 2048, 4096
    );

    manager.record_generation(
        "/models/another-model.gguf",
        55.0, 1800.0, 110, 2200, 4096
    );

    // Сериализация
    std::string json_str = manager.to_json();

    assert(!json_str.empty());
    assert(json_str.find("test-model") != std::string::npos);
    assert(json_str.find("another-model") != std::string::npos);
    assert(json_str.find("total_generations") != std::string::npos);

    // Десериализация
    ModelPerformanceManager manager2;
    bool success = manager2.from_json(json_str);

    assert(success);
    assert(manager2.get_model_count() == 2);

    auto stats = manager2.get_model_stats("/models/test-model.gguf");
    assert(stats.total_generations == 1);
    // После первой записи: avg = 0.1 * 45.0 = 4.5
    assert(stats.avg_tokens_per_second > 4.0 && stats.avg_tokens_per_second < 5.0);

    std::cout << "PASSED" << std::endl;
}

void test_model_performance_manager_report() {
    std::cout << "[TEST] ModelPerformanceManager report generation... ";

    ModelPerformanceManager manager;

    manager.record_generation(
        "/models/Qwen2.5-4B-Instruct.gguf",
        45.0, 2000.0, 100, 2048, 4096
    );

    manager.record_generation(
        "/models/Llama-3-8B.gguf",
        35.0, 2500.0, 90, 1800, 4096
    );

    std::string report = manager.generate_report();

    assert(!report.empty());
    assert(report.find("Model Performance Statistics") != std::string::npos);
    assert(report.find("Qwen2.5-4B-Instruct") != std::string::npos);
    assert(report.find("Llama-3-8B") != std::string::npos);
    assert(report.find("Total models tracked: 2") != std::string::npos);
    assert(report.find("Avg tokens/sec") != std::string::npos);

    std::cout << "PASSED" << std::endl;
}

void test_model_performance_manager_reset() {
    std::cout << "[TEST] ModelPerformanceManager reset... ";

    ModelPerformanceManager manager;

    manager.record_generation(
        "/models/test-model.gguf",
        45.0, 2000.0, 100, 2048, 4096
    );

    assert(manager.get_model_count() == 1);

    // Сброс конкретной модели
    manager.reset_model_stats("/models/test-model.gguf");
    assert(manager.get_model_count() == 0);
    assert(!manager.has_model_stats("/models/test-model.gguf"));

    // Тест сброса всех
    manager.record_generation(
        "/models/model-a.gguf",
        50.0, 2000.0, 100, 2048, 4096
    );
    manager.record_generation(
        "/models/model-b.gguf",
        30.0, 3000.0, 80, 1500, 4096
    );

    assert(manager.get_model_count() == 2);
    manager.reset_all_stats();
    assert(manager.get_model_count() == 0);

    std::cout << "PASSED" << std::endl;
}

void test_model_performance_sliding_average() {
    std::cout << "[TEST] ModelPerformanceManager sliding average... ";

    ModelPerformanceManager manager;

    // Записать много генераций с разными значениями
    for (int i = 0; i < 10; i++) {
        manager.record_generation(
            "/models/test-model.gguf",
            40.0 + i * 2,  // 40, 42, 44, ..., 58
            2000.0,
            100,
            2048,
            4096
        );
    }

    auto stats = manager.get_model_stats("/models/test-model.gguf");

    // Скользящее среднее (alpha=0.1) после 10 итераций будет около 52-54
    // (ближе к последним значениям, но с большим весом на ранних)
    assert(stats.avg_tokens_per_second > 30.0);
    assert(stats.avg_tokens_per_second < 58.0);
    assert(stats.total_generations == 10);

    std::cout << "PASSED" << std::endl;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    std::cout << "========================================" << std::endl;
    std::cout << "Model Performance Stats Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    try {
        test_model_performance_stats_basic();
        test_model_performance_manager_record();
        test_model_performance_manager_multiple_models();
        test_model_performance_manager_json();
        test_model_performance_manager_report();
        test_model_performance_manager_reset();
        test_model_performance_sliding_average();

        std::cout << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "All tests PASSED!" << std::endl;
        std::cout << "========================================" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test FAILED: " << e.what() << std::endl;
        return 1;
    }
}
