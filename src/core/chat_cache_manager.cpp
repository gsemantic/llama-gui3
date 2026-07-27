#include "../include/core/chat_cache_manager.h"
#include <cmath>
#include <algorithm>
#include <ctime>

namespace llama_gui {
namespace core {

ChatCacheManager::ChatCacheManager(int max_cache_size) 
    : max_cache_size_(max_cache_size) {}

std::string ChatCacheManager::find_best_match(const std::string& query, float threshold) {
    if (cache_entries_.empty() || !cache_enabled_) {
        return "";
    }
    
    // Генерируем эмбеддинг для нового запроса
    // Временная заглушка - в реальности нужно получить эмбеддинг через RagManager
    std::vector<float> query_embedding(384, 0.0f); // Размер all-MiniLM-L6-v2
    
    float best_similarity = 0.0f;
    std::string best_response = "";
    
    // Ищем наиболее похожий запрос в кэше
    for (const auto& entry : cache_entries_) {
        float similarity = cosine_similarity(query_embedding, entry.embedding);
        
        if (similarity > best_similarity && similarity >= threshold) {
            best_similarity = similarity;
            best_response = entry.response;
        }
    }
    
    operation_count_++;
    conditional_cleanup();
    
    return best_response;
}

void ChatCacheManager::add_to_cache(const std::string& query, const std::string& response) {
    if (!cache_enabled_) {
        return;
    }
    
    // Временная заглушка для генерации эмбеддинга
    std::vector<float> embedding(384, 0.0f); // Размер all-MiniLM-L6-v2
    
    CachedQuery new_entry;
    new_entry.query = query;
    new_entry.response = response;
    new_entry.embedding = embedding;
    new_entry.timestamp = static_cast<uint64_t>(time(nullptr));
    
    cache_entries_.push_back(new_entry);
    
    // Ограничиваем размер кэша
    if (static_cast<int>(cache_entries_.size()) > max_cache_size_) {
        // Удаляем самые старые записи
        int excess = static_cast<int>(cache_entries_.size()) - max_cache_size_;
        cache_entries_.erase(cache_entries_.begin(), 
                            cache_entries_.begin() + excess);
    }
    
    operation_count_++;
    conditional_cleanup();
}

float ChatCacheManager::cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return 0.0f;
    
    float dot_product = 0.0f;
    float norm_a = 0.0f;
    float norm_b = 0.0f;
    
    for (size_t i = 0; i < a.size(); ++i) {
        dot_product += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }
    
    if (norm_a == 0.0f || norm_b == 0.0f) {
        return 0.0f;
    }
    
    return dot_product / (sqrt(norm_a) * sqrt(norm_b));
}

void ChatCacheManager::normalize_vector(std::vector<float>& vec) {
    float norm = 0.0f;
    for (float val : vec) {
        norm += val * val;
    }
    norm = std::sqrt(norm);
    if (norm > 0.0f) {
        for (float& val : vec) {
            val /= norm;
        }
    }
}

void ChatCacheManager::cleanup_old_entries() {
    // Удаляем записи старше 24 часов
    auto now = static_cast<uint64_t>(time(nullptr));
    cache_entries_.erase(
        std::remove_if(cache_entries_.begin(), cache_entries_.end(),
            [now](const CachedQuery& entry) {
                return (now - entry.timestamp) > 86400; // 24 часа
            }),
        cache_entries_.end()
    );
}

void ChatCacheManager::conditional_cleanup() {
    // Очистка каждые N операций
    if (operation_count_ % CACHE_CLEANUP_INTERVAL == 0) {
        cleanup_old_entries();
    }
}

} // namespace core
} // namespace llama_gui