#include "../include/core/rag_manager.h"
#include "../include/core/embedding_generator.h"
#include <cmath>
#include <iostream>

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

std::vector<float> RagManager::generate_embedding(const std::string& text) {
    // Сначала проверяем кэш
    auto cached = get_cached_embedding(text);
    if (!cached.empty()) {
        return cached;
    }

    std::vector<float> embedding;

    // Генерируем новый эмбеддинг с помощью EmbeddingGenerator
    if (embedding_generator_ && embedding_generator_->is_loaded()) {
        embedding = embedding_generator_->generate_embedding(text);

        // Проверяем, что эмбеддинг был успешно сгенерирован
        if (embedding.empty()) {
            std::cerr << "Warning: Embedding generator returned empty vector" << std::endl;
        }
    } else {
        // Если генератор недоступен, используем резервный метод (например, случайные значения)
        std::cerr << "Warning: Embedding generator not available, using fallback" << std::endl;
        int embedding_dim = EMBEDDING_DIMENSION;
        if (embedding_generator_) {
            embedding_dim = embedding_generator_->get_embedding_dimension();
        }

        if (embedding_dim <= 0) {
            std::cerr << "Error: Invalid embedding dimension: " << embedding_dim << std::endl;
            return std::vector<float>(); // Возвращаем пустой вектор
        }

        embedding.resize(embedding_dim, 0.0f);

        // Заполняем случайными значениями для демонстрации
        // В реальной ситуации это может быть заменено на другой метод генерации
        for (size_t i = 0; i < embedding.size(); ++i) {
            embedding[i] = static_cast<float>(rand()) / RAND_MAX;
        }
    }

    // Нормализуем для косинусного сходства
    normalize_vector(embedding);

    // Кэшируем
    cache_embedding(text, embedding);

    return embedding;
}

std::string RagManager::build_rag_prompt(const std::string& user_query,
                                       const std::vector<RagChunk>& context_chunks,
                                       bool is_cloud_model) {
    // Формируем user-сообщение с контекстом и вопросом
    // Модель будет отвечать на этот вопрос

    // === ОПТИМИЗАЦИЯ РАЗМЕРА КОНТЕКСТА ===
    // Для облачных моделей - большие лимиты (128K контекст)
    // Для локальных моделей - компактный контекст
    constexpr size_t MAX_CONTEXT_CHARS_LOCAL = 4500;   // ~1024 токена (локальная модель)
    constexpr size_t MAX_CONTEXT_CHARS_CLOUD = 30000;  // ~8000 токенов (облачная модель)
    constexpr size_t MAX_CHUNKS_LOCAL = 10;
    constexpr size_t MAX_CHUNKS_CLOUD = 50;

    // Определяем лимиты в зависимости от типа модели
    size_t max_context_chars = is_cloud_model ? MAX_CONTEXT_CHARS_CLOUD : MAX_CONTEXT_CHARS_LOCAL;
    size_t max_chunks_limit = is_cloud_model ? MAX_CHUNKS_CLOUD : MAX_CHUNKS_LOCAL;

    // Вычисляем доступный размер на чанк
    size_t chunks_to_use = std::min(context_chunks.size(), max_chunks_limit);
    size_t max_chars_per_chunk = max_context_chars / chunks_to_use;
    
    // Минимум 200 символов на чанк для читаемости
    if (max_chars_per_chunk < 200) {
        max_chars_per_chunk = 200;
        chunks_to_use = std::min(chunks_to_use, max_context_chars / 200);
    }

    std::string prompt = "";
    prompt.reserve(max_context_chars + 256); // Предварительное выделение памяти

    // Добавляем контекст в компактном формате
    prompt += "=== КОНТЕКСТ ===\n";
    for (size_t i = 0; i < chunks_to_use; ++i) {
        // Обрезаем чанк до максимального размера
        std::string chunk = context_chunks[i].content;
        if (chunk.size() > max_chars_per_chunk) {
            // Находим последнюю полную UTF-8 последовательность
            size_t cut_pos = max_chars_per_chunk;
            while (cut_pos > 0 && (static_cast<unsigned char>(chunk[cut_pos]) & 0xC0) == 0x80) {
                cut_pos--;
            }
            chunk = chunk.substr(0, cut_pos) + "...";
        }
        prompt += "[" + std::to_string(i+1) + "] " + chunk + "\n";
    }
    prompt += "=== КОНЕЦ КОНТЕКСТА ===\n\n";

    // Явно формулируем вопрос
    prompt += "Вопрос: " + user_query + "\n";

    return prompt;
}

std::string RagManager::find_cached_response(const std::string& query) {
    // === ДВУХУРОВНЕВОЕ КЭШИРОВАНИЕ ===
    // Уровень 1: Точный кэш (полное совпадение) - порог 0.98
    // Уровень 2: Семантический кэш (похожие вопросы) - порог 0.85

    constexpr float EXACT_CACHE_THRESHOLD = 0.98f;
    constexpr float SEMANTIC_CACHE_THRESHOLD = 0.85f;

    // Генерируем эмбеддинг запроса для вычисления схожести
    auto query_embedding = generate_embedding(query);
    if (query_embedding.empty()) {
        return "";
    }

    // Ищем похожие запросы в чат-истории (возвращаем несколько кандидатов)
    auto similar_queries = search_chat_history(query, 5);

    if (!similar_queries.empty()) {
        // Уровень 1: Проверяем на точное совпадение
        for (const auto& chunk : similar_queries) {
            float similarity = cosine_similarity(query_embedding, chunk.embedding);

            if (similarity >= EXACT_CACHE_THRESHOLD) {
                return "Cached response for: " + chunk.content;
            }
        }

        // Уровень 2: Проверяем на семантическое совпадение
        for (const auto& chunk : similar_queries) {
            float similarity = cosine_similarity(query_embedding, chunk.embedding);
            if (similarity >= SEMANTIC_CACHE_THRESHOLD && similarity < EXACT_CACHE_THRESHOLD) {
                return "Cached response for: " + chunk.content;
            }
        }
    }

    return "";
}

void RagManager::cache_query_response(const std::string& query, const std::string& response) {
    // Добавляем запрос и ответ в чат-историю
    RagChunk query_chunk;
    query_chunk.content = query;
    query_chunk.document_id = "chat_history";
    query_chunk.chunk_index = static_cast<int>(chat_history_chunks_.size());
    query_chunk.embedding = generate_embedding(query);

    RagChunk response_chunk;
    response_chunk.content = response;
    response_chunk.document_id = "chat_history";
    response_chunk.chunk_index = static_cast<int>(chat_history_chunks_.size() + 1);
    response_chunk.embedding = generate_embedding(response);

    // Добавляем в индекс чат-истории
#ifdef USE_FAISS
    if (chat_history_index_) {
        std::vector<float> query_vec = query_chunk.embedding;
        std::vector<float> response_vec = response_chunk.embedding;
        normalize_vector(query_vec);
        normalize_vector(response_vec);

        long query_id = chat_history_chunks_.size();
        long response_id = chat_history_chunks_.size() + 1;

        chat_history_index_->add(1, query_vec.data());
        chat_history_index_->add(1, response_vec.data());
    }
#endif

    chat_history_chunks_.push_back(query_chunk);
    chat_history_chunks_.push_back(response_chunk);
}

} // namespace core
} // namespace llama_gui
