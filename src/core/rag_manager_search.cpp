#include "../include/core/rag_manager.h"
#include "../include/core/embedding_generator.h"
#include <iostream>
#include <algorithm>
#include <cmath>

#ifdef USE_FAISS
#include <faiss/IndexFlat.h>
#endif

namespace llama_gui {
namespace core {

// Вспомогательная функция для вычисления косинусного сходства
static float cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return 0.0f;
    float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }
    if (norm_a == 0 || norm_b == 0) return 0.0f;
    return dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
}

// Helper: filter chunks by path prefix
static std::vector<RagChunk> filter_by_path(
    const std::vector<RagChunk>& chunks, const std::string& path_filter)
{
    if (path_filter.empty()) return chunks;

    std::vector<RagChunk> filtered;
    filtered.reserve(chunks.size());
    for (const auto& chunk : chunks) {
        if (chunk.file_path.find(path_filter) != std::string::npos) {
            filtered.push_back(chunk);
        }
    }
    return filtered;
}

// MMR (Maximal Marginal Relevance) - выбор разнообразных результатов
static std::vector<std::pair<long, float>> apply_mmr(
    const std::vector<RagChunk>& chunks,
    const std::vector<float>& query_embedding,
    float lambda,
    int k)
{
    if (chunks.empty() || lambda <= 0.0f) {
        // Без MMR - возвращаем первые k
        std::vector<std::pair<long, float>> result;
        for (int i = 0; i < std::min(k, static_cast<int>(chunks.size())); ++i) {
            result.push_back({i, 1.0f});
        }
        return result;
    }

    std::vector<bool> selected(chunks.size(), false);
    std::vector<std::pair<long, float>> mmr_result;

    // Шаг 1: Выбираем первый элемент - самый релевантный запросу
    float max_relevance = -1.0f;
    int best_idx = 0;
    for (size_t i = 0; i < chunks.size(); ++i) {
        float sim = cosine_similarity(query_embedding, chunks[i].embedding);
        if (sim > max_relevance) {
            max_relevance = sim;
            best_idx = i;
        }
    }
    selected[best_idx] = true;
    mmr_result.push_back({best_idx, max_relevance});

    // Шаг 2: Итеративно выбираем остальные элементы
    while (static_cast<int>(mmr_result.size()) < k && mmr_result.size() < chunks.size()) {
        float max_mmr_score = -std::numeric_limits<float>::infinity();
        int best_next_idx = -1;

        for (size_t i = 0; i < chunks.size(); ++i) {
            if (selected[i]) continue;

            // Релевантность запросу
            float relevance = cosine_similarity(query_embedding, chunks[i].embedding);

            // Максимальное сходство с уже выбранными (штраф за дублирование)
            float max_sim_to_selected = 0.0f;
            for (const auto& selected_pair : mmr_result) {
                float sim = cosine_similarity(chunks[i].embedding, chunks[selected_pair.first].embedding);
                if (sim > max_sim_to_selected) {
                    max_sim_to_selected = sim;
                }
            }

            // MMR формула: λ * relevance - (1 - λ) * max_sim_to_selected
            float mmr_score = lambda * relevance - (1.0f - lambda) * max_sim_to_selected;

            if (mmr_score > max_mmr_score) {
                max_mmr_score = mmr_score;
                best_next_idx = i;
            }
        }

        if (best_next_idx >= 0) {
            selected[best_next_idx] = true;
            mmr_result.push_back({best_next_idx, max_mmr_score});
        } else {
            break;
        }
    }

    return mmr_result;
}

std::vector<RagChunk> RagManager::search_external_documents(const std::string& query, int k,
                                                             const std::string& path_filter) {
    // Проверяем, что запрос не пуст
    if (query.empty()) {
        std::cerr << "Warning: Empty query provided for external document search" << std::endl;
        return {};
    }

    // Проверяем, что k положительный
    if (k <= 0) {
        std::cerr << "Warning: Invalid k value for search: " << k << std::endl;
        return {};
    }

#ifdef USE_FAISS
    if (!external_docs_index_ || external_chunks_.empty()) {
        std::cerr << "Warning: External docs index not initialized or no chunks available" << std::endl;
        return {};
    }

    // Проверяем, что размерность эмбеддинга запроса соответствует размерности индекса
    auto query_embedding = generate_embedding(query);
    if (query_embedding.empty()) {
        std::cerr << "Error: Failed to generate embedding for query" << std::endl;
        return {};
    }

    if (static_cast<int>(query_embedding.size()) != external_docs_index_->d) {
        std::cerr << "Error: Query embedding dimension mismatch. Expected: " << external_docs_index_->d
                  << ", got: " << query_embedding.size() << std::endl;
        return {};
    }

    normalize_vector(query_embedding);

    // Подготавливаем векторы для результатов поиска
    // Для MMR ищем больше кандидатов, затем применяем MMR
    int search_k = k;
#ifdef USE_FAISS
    // Если MMR включен, ищем в 3 раза больше кандидатов для разнообразия
    if (enable_mmr_ && mmr_lambda_ > 0.0f) {
        search_k = std::min(k * 3, static_cast<int>(external_chunks_.size()));
    }
#endif

    std::vector<long> indices(search_k);
    std::vector<float> distances(search_k);

    try {
        // Поиск в FAISS
        external_docs_index_->search(1, query_embedding.data(), search_k, distances.data(), indices.data());
    } catch (const std::exception& e) {
        std::cerr << "Error: FAISS search failed: " << e.what() << std::endl;
        return {};
    }

    // Применяем MMR если включен
#ifdef USE_FAISS
    if (enable_mmr_ && mmr_lambda_ > 0.0f) {
        // Собираем кандидатов
        std::vector<RagChunk> candidates;
        for (int i = 0; i < search_k; ++i) {
            long idx = indices[i];
            if (idx >= 0 && idx < static_cast<long>(external_chunks_.size())) {
                if (distances[i] >= similarity_threshold_) {
                    candidates.push_back(external_chunks_[idx]);
                }
            }
        }

        // Применяем MMR
        auto mmr_result = apply_mmr(candidates, query_embedding, mmr_lambda_, k);

        // Собираем финальные результаты
        std::vector<RagChunk> results;
        for (const auto& pair : mmr_result) {
            results.push_back(candidates[pair.first]);
        }

        return results;
    }
#endif

    // Стандартный режим без MMR
    // Собираем результаты
    std::vector<RagChunk> results;
    for (int i = 0; i < search_k; ++i) {
        long idx = indices[i];
        if (idx >= 0 && idx < static_cast<long>(external_chunks_.size())) {
            // Проверяем порог схожести
            if (distances[i] >= similarity_threshold_) {
                results.push_back(external_chunks_[idx]);
            }
        }
        if (static_cast<int>(results.size()) >= k) break;
    }

    return filter_by_path(results, path_filter);
#else
    // Brute-force fallback: linear scan with cosine similarity
    if (external_chunks_.empty()) {
        return {};
    }

    auto query_embedding = generate_embedding(query);
    if (query_embedding.empty()) {
        return {};
    }

    normalize_vector(query_embedding);

    // Score all chunks
    std::vector<std::pair<int, float>> scored;
    scored.reserve(external_chunks_.size());
    for (size_t i = 0; i < external_chunks_.size(); ++i) {
        if (!external_chunks_[i].embedding.empty()) {
            float sim = cosine_similarity(query_embedding, external_chunks_[i].embedding);
            if (sim >= similarity_threshold_) {
                scored.push_back({static_cast<int>(i), sim});
            }
        }
    }

    // Sort by similarity descending
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    // Apply MMR if enabled
    if (enable_mmr_ && mmr_lambda_ > 0.0f && scored.size() > 1) {
        // Collect candidates
        std::vector<RagChunk> candidates;
        for (const auto& [idx, _] : scored) {
            candidates.push_back(external_chunks_[idx]);
        }
        auto mmr_result = apply_mmr(candidates, query_embedding, mmr_lambda_, k);
        std::vector<RagChunk> results;
        for (const auto& [idx, _] : mmr_result) {
            results.push_back(candidates[idx]);
        }
        return results;
    }

    // Standard mode: take top-k
    std::vector<RagChunk> results;
    for (const auto& [idx, _] : scored) {
        results.push_back(external_chunks_[idx]);
        if (static_cast<int>(results.size()) >= k) break;
    }
    return filter_by_path(results, path_filter);
#endif
}

std::vector<RagChunk> RagManager::search_chat_history(const std::string& query, int k) {
    // Проверяем, что запрос не пуст
    if (query.empty()) {
        std::cerr << "Warning: Empty query provided for chat history search" << std::endl;
        return {};
    }

    // Проверяем, что k положительный
    if (k <= 0) {
        std::cerr << "Warning: Invalid k value for search: " << k << std::endl;
        return {};
    }

#ifdef USE_FAISS
    // Аналогично поиску по внешним документам, но в другом индексе
    if (!chat_history_index_ || chat_history_chunks_.empty()) {
        std::cerr << "Warning: Chat history index not initialized or no chunks available" << std::endl;
        return {};
    }

    // Проверяем, что размерность эмбеддинга запроса соответствует размерности индекса
    auto query_embedding = generate_embedding(query);
    if (query_embedding.empty()) {
        std::cerr << "Error: Failed to generate embedding for query" << std::endl;
        return {};
    }

    if (static_cast<int>(query_embedding.size()) != chat_history_index_->d) {
        std::cerr << "Error: Query embedding dimension mismatch. Expected: " << chat_history_index_->d
                  << ", got: " << query_embedding.size() << std::endl;
        return {};
    }

    normalize_vector(query_embedding);

    // Подготавливаем векторы для результатов поиска
    std::vector<long> indices(k);
    std::vector<float> distances(k);

    try {
        // Поиск в FAISS
        chat_history_index_->search(1, query_embedding.data(), k, distances.data(), indices.data());
    } catch (const std::exception& e) {
        std::cerr << "Error: FAISS search failed for chat history: " << e.what() << std::endl;
        return {};
    }

    // Собираем результаты
    std::vector<RagChunk> results;
    for (int i = 0; i < k; ++i) {
        long idx = indices[i];
        if (idx >= 0 && idx < static_cast<long>(chat_history_chunks_.size())) {
            // ВАЖНО: Используем более высокий порог для кэша (0.95) чтобы избежать ложных совпадений
            float cache_threshold = 0.95f;
            if (distances[i] >= cache_threshold) {
                results.push_back(chat_history_chunks_[idx]);
            }
        }
    }

    return results;
#else
    // Brute-force fallback: linear scan with cosine similarity
    if (chat_history_chunks_.empty()) {
        return {};
    }

    auto query_embedding = generate_embedding(query);
    if (query_embedding.empty()) {
        return {};
    }

    normalize_vector(query_embedding);

    float cache_threshold = 0.95f;
    std::vector<std::pair<int, float>> scored;
    scored.reserve(chat_history_chunks_.size());
    for (size_t i = 0; i < chat_history_chunks_.size(); ++i) {
        if (!chat_history_chunks_[i].embedding.empty()) {
            float sim = cosine_similarity(query_embedding, chat_history_chunks_[i].embedding);
            if (sim >= cache_threshold) {
                scored.push_back({static_cast<int>(i), sim});
            }
        }
    }

    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::vector<RagChunk> results;
    for (const auto& [idx, _] : scored) {
        results.push_back(chat_history_chunks_[idx]);
        if (static_cast<int>(results.size()) >= k) break;
    }
    return results;
#endif
}

} // namespace core
} // namespace llama_gui
