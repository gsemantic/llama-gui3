#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <ctime>
#include "rag_manager.h"

namespace llama_gui {
namespace core {

struct CachedQuery {
    std::string query;
    std::string response;
    std::vector<float> embedding;
    uint64_t timestamp;
};

class ChatCacheManager {
public:
    ChatCacheManager(int max_cache_size = 300); // Уменьшенный размер для экономии памяти
    
    std::string find_best_match(const std::string& query, float threshold = 0.85f);
    void add_to_cache(const std::string& query, const std::string& response);
    bool is_cache_enabled() const { return cache_enabled_; }
    void set_cache_enabled(bool enabled) { cache_enabled_ = enabled; }
    
    // Методы для оптимизации
    void set_max_cache_size(int size) { max_cache_size_ = size; }
    size_t get_cache_size() const { return cache_entries_.size(); }
    void clear_cache() { cache_entries_.clear(); }
    
private:
    std::vector<CachedQuery> cache_entries_;
    int max_cache_size_;
    bool cache_enabled_ = true;
    int operation_count_ = 0;
    static constexpr int CACHE_CLEANUP_INTERVAL = 50; // Очистка каждые N операций
    
    // Очистка устаревших записей
    void cleanup_old_entries();
    void conditional_cleanup();
    
    // Вычисление косинусного сходства
    float cosine_similarity(const std::vector<float>& a, const std::vector<float>& b);
    
    // Нормализация вектора
    void normalize_vector(std::vector<float>& vec);
};

} // namespace core
} // namespace llama_gui