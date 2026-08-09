#include "../include/core/rag_manager.h"
#include "../include/core/embedding_generator.h"
#include <ctime>
#include <algorithm>
#include <iostream>

#ifdef USE_FAISS
#include <faiss/IndexFlat.h>
#endif

namespace llama_gui {
namespace core {

void RagManager::cleanup_old_chunks() {
    if (static_cast<int>(external_chunks_.size()) >= max_chunks_in_memory_) {
        int excess = static_cast<int>(external_chunks_.size()) - max_chunks_in_memory_ + 1;

        external_chunks_.erase(external_chunks_.begin(),
                              external_chunks_.begin() + excess);

        // Перестраиваем индекс с новыми данными
#ifdef USE_FAISS
        rebuild_index();
#endif
    }
}

void RagManager::rebuild_index() {
#ifdef USE_FAISS
    // Используем фактическую размерность текущего индекса (или из генератора),
    // а не жёстко заданную константу EMBEDDING_DIMENSION
    int dim = EMBEDDING_DIMENSION;
    if (external_docs_index_) {
        dim = static_cast<int>(external_docs_index_->d);
    } else if (embedding_generator_) {
        dim = embedding_generator_->get_embedding_dimension();
    }

    if (external_chunks_.empty()) {
        external_docs_index_ = create_optimized_index(dim);
        return;
    }

    // Пересоздаем индекс с текущими векторами
    auto new_index = create_optimized_index(dim);

    if (new_index) {
        std::vector<float> all_vectors;
        all_vectors.reserve(external_chunks_.size() * static_cast<size_t>(dim));

        for (const auto& chunk : external_chunks_) {
            if (static_cast<int>(chunk.embedding.size()) == dim) {
                all_vectors.insert(all_vectors.end(),
                                 chunk.embedding.begin(),
                                 chunk.embedding.end());
            }
        }

        if (!all_vectors.empty()) {
            size_t count = all_vectors.size() / static_cast<size_t>(dim);
            new_index->add(static_cast<int>(count), all_vectors.data());
        }

        external_docs_index_ = std::move(new_index);
    }
#endif
}

std::vector<float> RagManager::get_cached_embedding(const std::string& text) {
    auto it = query_embedding_cache_.find(text);
    if (it != query_embedding_cache_.end()) {
        it->second.last_access = static_cast<uint64_t>(time(nullptr));
        return it->second.embedding;
    }
    return {};
}

void RagManager::cache_embedding(const std::string& text, const std::vector<float>& embedding) {
    // Проверяем, есть уже такой ключ
    auto existing = query_embedding_cache_.find(text);
    if (existing != query_embedding_cache_.end()) {
        existing->second.embedding = embedding;
        existing->second.last_access = static_cast<uint64_t>(time(nullptr));
        return;
    }

    // LRU-вытеснение: удаляем самый старый элемент при переполнении
    if (static_cast<int>(query_embedding_cache_.size()) >= max_embedding_cache_size_) {
        auto oldest_it = query_embedding_cache_.end();
        uint64_t oldest_time = UINT64_MAX;
        
        for (auto it = query_embedding_cache_.begin(); it != query_embedding_cache_.end(); ++it) {
            if (it->second.last_access < oldest_time) {
                oldest_time = it->second.last_access;
                oldest_it = it;
            }
        }
        
        if (oldest_it != query_embedding_cache_.end()) {
            query_embedding_cache_.erase(oldest_it);
        }
    }

    // Вставляем новый элемент
    query_embedding_cache_.emplace(text, QueryEmbeddingCache{embedding, static_cast<uint64_t>(time(nullptr))});
}

void RagManager::cleanup_embedding_cache() {
    uint64_t now = static_cast<uint64_t>(time(nullptr));
    uint64_t max_age = 3600; // 1 hour

    for (auto it = query_embedding_cache_.begin(); it != query_embedding_cache_.end(); ) {
        if (now - it->second.last_access > max_age) {
            it = query_embedding_cache_.erase(it);
        } else {
            ++it;
        }
    }
}

void RagManager::set_max_chunks(int max_chunks) {
    max_chunks_in_memory_ = std::min(max_chunks, 10000); // Ограничиваем максимальное значение
}

void RagManager::set_similarity_threshold(float threshold) {
    similarity_threshold_ = threshold;
}

void RagManager::set_embedding_server_url(const std::string& url) {
    embedding_server_url_ = url;
    if (embedding_generator_) {
        embedding_generator_->set_server_url(url);
        std::cout << "[RAG] Embedding server URL set to: " << url << std::endl;
    }
}

void RagManager::update_from_settings(const core::RagSettings& settings) {
    max_chunks_in_memory_ = settings.max_chunks_in_memory;
    similarity_threshold_ = settings.similarity_threshold;
    max_embedding_cache_size_ = settings.max_embedding_cache_size;
    max_tokens_per_chunk_ = settings.max_tokens_per_chunk;
    enable_mmr_ = settings.enable_mmr;  // MMR enable
    mmr_lambda_ = settings.mmr_lambda;  // MMR lambda
    enable_kv_cache_ = settings.enable_kv_cache;  // KV-cache enable
    
    // Параметры гибридного поиска
    enable_hybrid_search_ = settings.enable_hybrid_search;
    keyword_boost_weight_ = settings.keyword_boost_weight;
    enable_query_expansion_ = settings.enable_query_expansion;

    // URL сервера эмбеддингов
    if (!settings.embedding_server_url.empty()) {
        set_embedding_server_url(settings.embedding_server_url);
    }

    // Размерность эмбеддингов из настроек (если задана)
    if (settings.embedding_dimension > 0 && embedding_generator_) {
        embedding_generator_->set_embedding_dimension(settings.embedding_dimension);
    }

    // Embedding Proxy
    if (settings.enable_embedding_proxy) {
        if (!is_embedding_proxy_running()) {
            start_embedding_proxy(settings.embedding_proxy_port);
        }
    } else {
        if (is_embedding_proxy_running()) {
            stop_embedding_proxy();
        }
    }
}

} // namespace core
} // namespace llama_gui
