#pragma once

#include <string>

namespace llama_gui {
namespace core {

// Режимы работы RAG
enum class RagMode {
    DocumentsOnly,    // Только поиск по загруженным документам (рекомендуется)
    CacheOnly,        // Только кэш чат-истории
    Both              // Оба режима (документы + кэш)
};

// Режимы глубокого анализа документа
enum class DeepAnalysisMode {
    Disabled,         // Отключен (обычный RAG поиск)
    MapReduce,        // Map-Reduce: разбиение на группы → резюме → синтез
    Iterative,        // Итеративный: последовательный обход всех чанков
    Hierarchical      // Иерархический: дерево резюме (для очень больших документов)
};

// Настройки глубокого анализа
struct DeepAnalysisSettings {
    DeepAnalysisMode mode = DeepAnalysisMode::Disabled;  // Режим глубокого анализа
    int chunks_per_batch = 10;  // Количество чанков в одном батче (для Map-Reduce)
    int max_iterations = 50;    // Максимальное количество итераций
    bool enable_progressive_summary = true;  // Включить прогрессивное суммирование
    int final_synthesis_chunks = 30;  // Количество резюме для финального синтеза
    bool auto_adjust_context_size = true;  // Автоматически увеличивать контекст сервера
    int target_context_size = 8192;  // Целевой размер контекста (если auto_adjust=true)
};

struct RagSettings {
    std::string embedding_model_path = "";  // Пустой путь - пользователь выбирает в настройках
    std::string embedding_server_url = "";  // URL сервера для /v1/embeddings (пусто = локальная модель)
    int max_chunks_in_memory = 100;
    float similarity_threshold = 0.70f;
    int max_embedding_cache_size = 50;
    int embedding_dimension = 384; // granite-embedding-107m-multilingual
    int max_sequence_length = 512;
    int max_tokens_per_chunk = 256;
    int search_k = 5;
    float mmr_lambda = 0.5f;  // MMR параметр: 0=только релевантность, 1=только разнообразие
    bool enable_mmr = false;  // Включить MMR для разнообразия результатов
    bool enable_rag = true;
    bool enable_caching = true;
    bool enable_kv_cache = true;  // Включить KV-cache для документов

    // Параметры гибридного поиска
    bool enable_hybrid_search = true;  // Включить гибридный поиск (векторный + полнотекстовый)
    float keyword_boost_weight = 2.0f;
    bool enable_query_expansion = true;  // Включить расширение запроса (транслитерация)

    // Настройки глубокого анализа документа
    DeepAnalysisSettings deep_analysis;

    // Embedding Proxy settings
    bool enable_embedding_proxy = false;  // Включить embedding proxy (OpenAI-compatible)
    int embedding_proxy_port = 8082;     // Порт прокси-сервера

    RagMode rag_mode = RagMode::DocumentsOnly;  // Режим работы RAG

    // Настройки бесшовного расширения контекста
    bool enable_context_extension = true;  // Включить бесшовное расширение контекста
    int context_compression_threshold = 85;  // Порог сжатия (в процентах)
    int keep_recent_messages = 4;  // Количество последних сообщений без суммаризации
};

} // namespace core
} // namespace llama_gui