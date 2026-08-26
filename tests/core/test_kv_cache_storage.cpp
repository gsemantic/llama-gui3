/**
 * @file test_kv_cache_storage.cpp
 * @brief Unit тесты для KVCacheStorage
 */

#include "../include/core/kv_cache_storage.h"
#include "../include/core/kv_cache_types.h"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace llama_gui::core;

// ============================================================================
// Тесты
// ============================================================================

std::string get_test_cache_path() {
    return "/tmp/llama-gui-kv-cache-test";
}

void cleanup_test_cache() {
    try {
        if (fs::exists(get_test_cache_path())) {
            fs::remove_all(get_test_cache_path());
        }
    } catch (...) {}
}

void test_storage_creation() {
    std::cout << "[TEST] Creating KVCacheStorage... ";
    cleanup_test_cache();

    auto storage = std::make_unique<KVCacheStorage>(get_test_cache_path());
    assert(storage != nullptr);
    assert(storage->get_base_path() == get_test_cache_path());

    cleanup_test_cache();
    std::cout << "PASSED" << std::endl;
}

void test_directory_creation() {
    std::cout << "[TEST] Directory auto-creation... ";
    cleanup_test_cache();

    {
        auto storage = std::make_unique<KVCacheStorage>(get_test_cache_path());
        assert(fs::exists(get_test_cache_path()));
        assert(fs::is_directory(get_test_cache_path()));
    }

    cleanup_test_cache();
    std::cout << "PASSED" << std::endl;
}

void test_get_cache_path() {
    std::cout << "[TEST] Getting cache path... ";
    cleanup_test_cache();

    auto storage = std::make_unique<KVCacheStorage>(get_test_cache_path());
    std::string doc_id = "test_document_123";
    std::string path = storage->get_cache_path(doc_id);

    // Путь должен содержать базовую директорию и doc_id
    assert(path.find(get_test_cache_path()) == 0);
    assert(path.find(".bin") != std::string::npos);

    cleanup_test_cache();
    std::cout << "PASSED" << std::endl;
}

void test_has_cache() {
    std::cout << "[TEST] Checking cache existence... ";
    cleanup_test_cache();

    auto storage = std::make_unique<KVCacheStorage>(get_test_cache_path());
    std::string doc_id = "test_doc";

    // Сначала файла нет
    assert(storage->has_cache(doc_id) == false);

    // Создаём фиктивный файл
    std::string path = storage->get_cache_path(doc_id);
    std::ofstream file(path);
    file << "test data";
    file.close();

    // Теперь файл есть
    assert(storage->has_cache(doc_id) == true);

    cleanup_test_cache();
    std::cout << "PASSED" << std::endl;
}

void test_delete_cache() {
    std::cout << "[TEST] Deleting cache... ";
    cleanup_test_cache();

    auto storage = std::make_unique<KVCacheStorage>(get_test_cache_path());
    std::string doc_id = "test_doc";

    // Создаём файл
    std::string path = storage->get_cache_path(doc_id);
    std::ofstream file(path);
    file << "test data";
    file.close();

    // Удаляем
    bool deleted = storage->delete_cache(doc_id);
    assert(deleted == true);
    assert(fs::exists(path) == false);

    // Повторное удаление должно вернуть false
    deleted = storage->delete_cache(doc_id);
    assert(deleted == false);

    cleanup_test_cache();
    std::cout << "PASSED" << std::endl;
}

void test_get_cache_size() {
    std::cout << "[TEST] Getting cache size... ";
    cleanup_test_cache();

    auto storage = std::make_unique<KVCacheStorage>(get_test_cache_path());
    std::string doc_id = "test_doc";

    // Создаём файл известного размера
    std::string path = storage->get_cache_path(doc_id);
    std::string test_data = "0123456789ABCDEF";  // 16 байт
    std::ofstream file(path, std::ios::binary);
    file.write(test_data.c_str(), test_data.size());
    file.close();

    size_t size = storage->get_cache_size(doc_id);
    assert(size == test_data.size());

    // Для несуществующего файла размер должен быть 0
    size_t missing_size = storage->get_cache_size("missing_doc");
    assert(missing_size == 0);

    cleanup_test_cache();
    std::cout << "PASSED" << std::endl;
}

void test_cleanup_old_caches() {
    std::cout << "[TEST] Cleaning up old caches... ";
    cleanup_test_cache();

    auto storage = std::make_unique<KVCacheStorage>(get_test_cache_path());

    // Создаём несколько файлов
    for (int i = 0; i < 5; i++) {
        std::string doc_id = "doc_" + std::to_string(i);
        std::string path = storage->get_cache_path(doc_id);
        std::ofstream file(path);
        file << "data " << i;
        file.close();
    }

    // Проверяем что файлы созданы
    assert(storage->get_file_count() == 5);

    // Очищаем все (older_than_seconds = 0)
    int deleted = storage->cleanup_old_caches(0);
    assert(deleted == 5);
    assert(storage->get_file_count() == 0);

    cleanup_test_cache();
    std::cout << "PASSED" << std::endl;
}

void test_clear_all_caches() {
    std::cout << "[TEST] Clearing all caches... ";
    cleanup_test_cache();

    auto storage = std::make_unique<KVCacheStorage>(get_test_cache_path());

    // Создаём несколько файлов
    for (int i = 0; i < 3; i++) {
        std::string doc_id = "doc_" + std::to_string(i);
        std::string path = storage->get_cache_path(doc_id);
        std::ofstream file(path);
        file << "data " << i;
        file.close();
    }

    // Очищаем всё
    int deleted = storage->clear_all_caches();
    assert(deleted == 3);
    assert(storage->get_file_count() == 0);

    cleanup_test_cache();
    std::cout << "PASSED" << std::endl;
}

void test_get_all_cached_documents() {
    std::cout << "[TEST] Getting all cached documents... ";
    cleanup_test_cache();

    auto storage = std::make_unique<KVCacheStorage>(get_test_cache_path());

    // Создаём файлы
    std::vector<std::string> doc_ids = {"doc_a", "doc_b", "doc_c"};
    for (const auto& doc_id : doc_ids) {
        std::string path = storage->get_cache_path(doc_id);
        std::ofstream file(path);
        file << "data";
        file.close();
    }

    auto cached_docs = storage->get_all_cached_documents();
    assert(cached_docs.size() == 3);

    cleanup_test_cache();
    std::cout << "PASSED" << std::endl;
}

void test_get_total_storage_size() {
    std::cout << "[TEST] Getting total storage size... ";
    cleanup_test_cache();

    auto storage = std::make_unique<KVCacheStorage>(get_test_cache_path());

    // Создаём файлы известного размера
    size_t file_size = 100;
    for (int i = 0; i < 3; i++) {
        std::string doc_id = "doc_" + std::to_string(i);
        std::string path = storage->get_cache_path(doc_id);
        std::ofstream file(path, std::ios::binary);
        std::vector<char> data(file_size, 'x');
        file.write(data.data(), data.size());
        file.close();
    }

    size_t total_size = storage->get_total_storage_size();
    assert(total_size == file_size * 3);

    cleanup_test_cache();
    std::cout << "PASSED" << std::endl;
}

void test_is_writable() {
    std::cout << "[TEST] Checking if writable... ";
    cleanup_test_cache();

    auto storage = std::make_unique<KVCacheStorage>(get_test_cache_path());
    bool writable = storage->is_writable();
    assert(writable == true);

    cleanup_test_cache();
    std::cout << "PASSED" << std::endl;
}

void test_get_available_space() {
    std::cout << "[TEST] Getting available space... ";
    cleanup_test_cache();

    auto storage = std::make_unique<KVCacheStorage>(get_test_cache_path());
    size_t available = storage->get_available_space();

    // Должно быть больше 0 (хотя бы какой-то объём)
    assert(available > 0);

    cleanup_test_cache();
    std::cout << "PASSED" << std::endl;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    std::cout << "========================================" << std::endl;
    std::cout << "KV-Cache Storage Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    try {
        test_storage_creation();
        test_directory_creation();
        test_get_cache_path();
        test_has_cache();
        test_delete_cache();
        test_get_cache_size();
        test_cleanup_old_caches();
        test_clear_all_caches();
        test_get_all_cached_documents();
        test_get_total_storage_size();
        test_is_writable();
        test_get_available_space();

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
