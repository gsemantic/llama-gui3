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

    /* R1.3: общий генератор важнее собственного. Порядок не косметический:
     * после set_shared_embedding_generator() собственный сброшен, но если
     * передали nullptr — работает свой (прежнее поведение). */
    EmbeddingGenerator* gen = active_generator();

    // R0.1: без рабочего генератора возвращаем пустой вектор.
    //
    // Раньше здесь был «fallback на случайные значения» (rand()/RAND_MAX):
    // он давал вектор с ненулевой нормой, который проходил нормализацию и
    // добавлялся в индекс. Косинусное сходство между двумя случайными
    // векторами большой размерности мало (≈ 1/√dim), но НЕ равно нулю,
    // и при поиске по индексу, заполненному такими векторами, поиск
    // возвращал результаты — правдоподобные и одновременно бесполезные.
    // Это худший вид отказа: вызывающий видит «результат есть» и не
    // понимает, что эмбеддингов нет.
    //
    // Пустой вектор — однозначный сигнал: вызывающие (search_*,
    // process_text_chunk, host_rag_embedding) уже обрабатывают его
    // правильно (возвращают пусто или 0). Причина отсутствия логируется
    // один раз — в конструкторе RagManager, когда embedding_generator_
    // сбрасывается после неудачного load_model().
    if (!gen || !gen->is_loaded()) {
        return {};
    }

    std::vector<float> embedding = gen->generate_embedding(text);
    if (embedding.empty()) {
        // Генератор был загружен, но не смог выдать эмбеддинг (например,
        // сервер вернул ошибку и локальный fallback тоже не сработал).
        // Логируем только один раз на процесс, чтобы не засорять лог
        // при массовой индексации.
        static bool logged_once = false;
        if (!logged_once) {
            std::cerr << "[RAG] Embedding generator returned empty vector — "
                         "embeddings will be unavailable until the generator recovers"
                      << std::endl;
            logged_once = true;
        }
        return {};
    }

    // Нормализуем для косинусного сходства
    normalize_vector(embedding);

    // Кэшируем (только непустые)
    cache_embedding(text, embedding);

    return embedding;
}

std::string RagManager::build_rag_prompt(const std::string& user_query,
                                       const std::vector<RagChunk>& context_chunks,
                                       bool is_cloud_model) {
    // Формируем user-сообщение с контекстом и вопросом
    // Модель будет отвечать на этот вопрос

    // R0.1 (попутно): без контекста просто формулируем вопрос. Раньше при
    // пустых chunks строка `max_context_chars / chunks_to_use` давала
    // деление на ноль; это баг, смежный с задачей «при пустом векторе
    // возвращать пустой результат, а не мусор».
    if (context_chunks.empty()) {
        return "Вопрос: " + user_query + "\n";
    }

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
                return cached_response_for_chunk(chunk);
            }
        }

        // Уровень 2: Проверяем на семантическое совпадение
        for (const auto& chunk : similar_queries) {
            float similarity = cosine_similarity(query_embedding, chunk.embedding);
            if (similarity >= SEMANTIC_CACHE_THRESHOLD && similarity < EXACT_CACHE_THRESHOLD) {
                return cached_response_for_chunk(chunk);
            }
        }
    }

    return "";
}

// Возвращает настоящий ответ, соответствующий найденному фрагменту чат-истории.
// Запросы и ответы хранятся парами: чётный chunk_index — запрос, нечётный — ответ.
// Если совпадение пришлось на сам ответ — возвращаем его, если на запрос — парный ответ.
std::string RagManager::cached_response_for_chunk(const RagChunk& chunk) {
    if (chunk.chunk_index % 2 == 1) {
        return chunk.content;
    }
    if (chunk.chunk_index + 1 < chat_history_chunks_.size()) {
        return chat_history_chunks_[chunk.chunk_index + 1].content;
    }
    return chunk.content;
}

void RagManager::cache_query_response(const std::string& query, const std::string& response) {
    // R0.1: генерируем эмбеддинги ДО создания чанков, чтобы не записывать
    // чанки с пустыми эмбеддингами в индекс. Пустой эмбеддинг означает
    // «генератор недоступен»; индекс с такими чанками превращается в
    // хранилище мусора, из которого search_chat_history никогда ничего
    // не вернёт, но и не скажет, что причина — отсутствие эмбеддингов.
    std::vector<float> query_embedding = generate_embedding(query);
    std::vector<float> response_embedding = generate_embedding(response);

    if (query_embedding.empty() || response_embedding.empty()) {
        // Не пишем в индекс: без эмбеддингов чанк бесполезен для поиска,
        // а его presence в chat_history_chunks_ создавало бы иллюзию,
        // что кэш работает.
        return;
    }

    RagChunk query_chunk;
    query_chunk.content = query;
    query_chunk.document_id = "chat_history";
    query_chunk.chunk_index = static_cast<int>(chat_history_chunks_.size());
    query_chunk.embedding = std::move(query_embedding);

    RagChunk response_chunk;
    response_chunk.content = response;
    response_chunk.document_id = "chat_history";
    response_chunk.chunk_index = static_cast<int>(chat_history_chunks_.size()) + 1;
    response_chunk.embedding = std::move(response_embedding);

    // Добавляем в индекс чат-истории
#ifdef USE_FAISS
    if (chat_history_index_) {
        std::vector<float> query_vec = query_chunk.embedding;
        std::vector<float> response_vec = response_chunk.embedding;
        normalize_vector(query_vec);
        normalize_vector(response_vec);

        // Размерность обязана совпадать с размерностью индекса. Иначе
        // faiss::IndexFlat::add читает за пределами буфера (segfault) или
        // портит индекс. Проверка не избыточна: размерность меняется при
        // смене модели эмбеддингов, а chat_history_index_ создаётся один
        // раз в конструкторе по размерности генератора.
        if (static_cast<int>(query_vec.size()) == chat_history_index_->d &&
            static_cast<int>(response_vec.size()) == chat_history_index_->d) {
            chat_history_index_->add(1, query_vec.data());
            chat_history_index_->add(1, response_vec.data());
        } else {
            std::cerr << "[RAG] Skipping chat_history index add: dimension mismatch ("
                      << query_vec.size() << "/" << response_vec.size()
                      << " vs " << chat_history_index_->d << ")" << std::endl;
        }
    }
#endif

    chat_history_chunks_.push_back(std::move(query_chunk));
    chat_history_chunks_.push_back(std::move(response_chunk));
}

} // namespace core
} // namespace llama_gui
