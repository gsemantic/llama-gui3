#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <ctime>
#include "llama_interface.h"
#include "rag_settings.h"
#include "kv_cache_types.h"
#include "rag_index_profile.h"
#include <functional>

// Предварительное объявление класса EmbeddingGenerator
namespace llama_gui {
namespace core {
    class EmbeddingGenerator;
    class EmbeddingProxy;
}
}

#ifdef USE_FAISS
namespace faiss {
    class Index;
}
#endif

namespace llama_gui {
namespace core {

// === Indexing Progress Tracking ===
enum class IndexingPhase {
    Idle,
    Parsing,           // Parsing document (txt/md/pdf/docx/code)
    Chunking,          // Splitting into chunks
    Embedding,         // Generating embeddings for chunks
    IndexingFAISS,     // Adding vectors to FAISS index
    Saving,            // Saving index to disk
    Complete,          // Indexing finished successfully
    Error              // Indexing failed
};

struct IndexingProgress {
    IndexingPhase phase = IndexingPhase::Idle;
    std::string current_file;        // Currently processing file
    int current_file_index = 0;      // 0-based index of current file
    int total_files = 0;             // Total files to process
    int current_chunk = 0;           // Current chunk being embedded
    int total_chunks = 0;            // Total chunks in current file
    int total_chunks_all = 0;        // Total chunks across all files
    int completed_chunks = 0;        // Chunks completed so far
    float overall_progress = 0.0f;   // 0.0 - 1.0
    std::string status_message;      // Human-readable status
    std::string error_message;       // Error details (if phase == Error)

    // Timing
    std::chrono::steady_clock::time_point start_time;
    double elapsed_seconds = 0.0;

    // Final stats
    int final_file_count = 0;
    int final_chunk_count = 0;
    size_t final_index_size_bytes = 0;
};

using IndexingProgressCallback = std::function<void(const IndexingProgress&)>;

struct RagChunk {
    std::string content;
    std::string document_id;
    int chunk_index;
    std::vector<float> embedding;

    // Code-aware metadata
    enum class ContentType { Text, Code, Mixed };
    ContentType content_type = ContentType::Text;
    std::string language;           // "cpp", "python", "rust", ""
    std::string file_path;          // путь к исходному файлу
    std::string symbol_name;        // имя функции/класса/метода
    std::string parent_symbol;      // родительский контекст (класс для метода)
    int start_line = 0;
    int end_line = 0;
};

class RagManager {
public:
    RagManager(const std::string& embedding_model_path = "models/embedding_model.gguf");
    ~RagManager();

    // Инициализация индексов
    bool initialize_indexes();

    // Обработка внешних документов
    bool process_document(const std::string& file_path, bool report_completion = true);
    bool process_documents_batch(const std::vector<std::string>& file_paths);
    bool process_text_chunk(const std::string& text, const std::string& doc_id, int chunk_index);
    bool reindex_document(const std::string& file_path);
    bool remove_document(const std::string& file_path);

    // Кэширование чат-истории
    std::string find_cached_response(const std::string& query);
    void cache_query_response(const std::string& query, const std::string& response);

    // Поиск по внешним документам
    std::vector<RagChunk> search_external_documents(const std::string& query, int k = 5,
                                                     const std::string& path_filter = "");
    
    // === ГИБРИДНЫЙ ПОИСК (векторный + полнотекстовый) ===
    std::vector<RagChunk> search_hybrid(const std::string& query, int k = 5,
                                         const std::string& path_filter = "");
    std::vector<RagChunk> rerank_results(const std::string& query, 
                                         const std::vector<RagChunk>& results,
                                         const std::vector<std::string>& keywords);
    std::vector<std::string> expand_query(const std::string& query);
    float keyword_boost_score(const RagChunk& chunk, const std::vector<std::string>& keywords);

    // Поиск по чат-истории
    std::vector<RagChunk> search_chat_history(const std::string& query, int k = 3);

    // Генерация эмбеддинга
    std::vector<float> generate_embedding(const std::string& text);

    // Формирование промпта с RAG-контекстом
    std::string build_rag_prompt(const std::string& user_query,
                                const std::vector<RagChunk>& context_chunks,
                                bool is_cloud_model = false);

    // === ГЛУБОКИЙ АНАЛИЗ ДОКУМЕНТА (Map-Reduce) ===
    
    /**
     * @brief Основной метод глубокого анализа с выбором режима
     * @param query Исходный запрос пользователя
     * @param all_chunks Все найденные чанки документов
     * @param settings Настройки глубокого анализа
     * @return Финальный синтезированный ответ
     */
    std::string process_deep_analysis(const std::string& query,
                                      std::vector<RagChunk>& all_chunks,
                                      const DeepAnalysisSettings& settings);

    /**
     * @brief Map-Reduce режим: разбиение на батчи → резюме → синтез
     * @param query Исходный запрос пользователя
     * @param all_chunks Все найденные чанки документов
     * @param settings Настройки глубокого анализа
     * @return Финальный синтезированный ответ
     */
    std::string process_deep_analysis_mapreduce(const std::string& query,
                                                 std::vector<RagChunk>& all_chunks,
                                                 const DeepAnalysisSettings& settings);

    /**
     * @brief Итеративный режим: последовательный обход всех чанков
     * @param query Исходный запрос пользователя
     * @param all_chunks Все найденные чанки документов
     * @param settings Настройки глубокого анализа
     * @return Финальный синтезированный ответ
     */
    std::string process_deep_analysis_iterative(const std::string& query,
                                                 std::vector<RagChunk>& all_chunks,
                                                 const DeepAnalysisSettings& settings);

    /**
     * @brief Иерархический режим: дерево резюме (для очень больших документов)
     * @param query Исходный запрос пользователя
     * @param all_chunks Все найденные чанки документов
     * @param settings Настройки глубокого анализа
     * @return Финальный синтезированный ответ
     */
    std::string process_deep_analysis_hierarchical(const std::string& query,
                                                    std::vector<RagChunk>& all_chunks,
                                                    const DeepAnalysisSettings& settings);

    /**
     * @brief Генерация резюме для одного чанка (Map-этап)
     * @param chunk Чанк для суммаризации
     * @param query Исходный запрос (для фокусировки резюме)
     * @return Текст резюме
     */
    std::string generate_chunk_summary(const RagChunk& chunk, const std::string& query);

    /**
     * @brief Синтез финального ответа из промежуточных резюме (Reduce-этап)
     * @param query Исходный запрос пользователя
     * @param summaries Список промежуточных резюме
     * @param target_context_size Целевой размер контекста
     * @return Финальный синтезированный ответ
     */
    std::string synthesize_final_answer(const std::string& query,
                                        const std::vector<std::string>& summaries,
                                        int target_context_size);

    /**
     * @brief Автоматическое увеличение размера контекста сервера
     * @param target_context_size Целевой размер контекста
     * @return true если успешно
     */
    bool auto_adjust_server_context_size(int target_context_size);

    // Управление параметрами
    void set_max_chunks(int max_chunks);
    void set_similarity_threshold(float threshold);
    void set_embedding_server_url(const std::string& url);

    // === Indexing Progress Callback ===
    void set_indexing_progress_callback(IndexingProgressCallback callback);
    IndexingProgress get_current_indexing_progress() const;

    // === Embedding Proxy ===
    bool start_embedding_proxy(int port = 8082);
    void stop_embedding_proxy();
    bool is_embedding_proxy_running() const;

    // Chunking utilities (public for testing)
    std::vector<std::string> split_into_chunks(const std::string& text, int max_tokens);
    int count_tokens_approx(const std::string& text);
    
    // Обновление из настроек
    void update_from_settings(const core::RagSettings& settings);

    // === Персистентность (сохранение/загрузка индекса) ===
    bool save_index(const std::string& index_path = "");
    bool load_index(const std::string& index_path = "");
    bool has_persistent_index() const;
    std::string get_default_index_path() const;
    void clear_all_indexes();

    // === Проверка совместимости индекса с текущей моделью эмбеддингов ===
    /// true, если загруженный индекс построен другой моделью/размерностью
    /// и требуется переиндексация для корректного поиска
    bool needs_reindex() const { return index_needs_reindex_; }
    /// Имя модели, которой построен текущий индекс (из metadata)
    std::string get_index_embedding_model() const { return index_embedding_model_; }
    /// Имя текущей модели эмбеддингов
    std::string get_active_embedding_model() const { return embedding_model_path_; }
    /// Причина необходимости переиндексации (для уведомления пользователя)
    std::string get_reindex_reason() const { return reindex_reason_; }
    /// Сбросить флаг после переиндексации
    void reset_needs_reindex() { index_needs_reindex_ = false; reindex_reason_.clear(); }

    // === Управление профилями индексов ===
    bool initialize_profile_manager(const std::string& profiles_directory = "");
    bool create_index_profile(const std::string& profile_name, const std::string& source_directory = "");
    bool switch_index_profile(const std::string& profile_name);
    bool delete_index_profile(const std::string& profile_name, bool delete_index_file = false);
    std::vector<std::string> get_index_profile_names() const;
    std::string get_current_index_profile() const;
    std::string get_current_index_path() const;  // Путь к файлу индекса текущего профиля
    std::string get_current_profile_source_directory() const;
    bool load_index_for_current_profile();
    bool reindex_current_profile();
    /// Список документов текущего профиля (пути к файлам)
    std::vector<std::string> get_current_profile_documents() const;

    // === KV-cache persistence для документов ===

    /**
     * @brief Сохранить KV-cache для обработанного документа
     * @param doc_id Уникальный идентификатор документа
     * @param slot_id ID слота, в котором загружен документ
     * @return true если успешно
     */
    bool save_document_kv_cache(const std::string& doc_id, int slot_id);

    /**
     * @brief Загрузить KV-cache для документа в слот
     * @param doc_id Уникальный идентификатор документа
     * @param slot_id ID слота для загрузки
     * @return true если успешно
     */
    bool load_document_kv_cache(const std::string& doc_id, int slot_id);

    /**
     * @brief Проверить наличие сохранённого KV-cache для документа
     * @param doc_id Уникальный идентификатор документа
     * @return true если файл существует
     */
    bool has_document_kv_cache(const std::string& doc_id) const;

    /**
     * @brief Удалить сохранённый KV-cache документа
     * @param doc_id Уникальный идентификатор документа
     * @return true если успешно
     */
    bool delete_document_kv_cache(const std::string& doc_id);

    /**
     * @brief Получить путь к файлу KV-cache для документа
     * @param doc_id Уникальный идентификатор документа
     * @return Полный путь к файлу
     */
    std::string get_document_kv_cache_path(const std::string& doc_id) const;

    /**
     * @brief Очистить все сохранённые KV-cache
     * @param older_than_seconds Удалить файлы старше указанного времени (0 = все)
     * @return Количество удалённых файлов
     */
    int cleanup_old_kv_caches(int older_than_seconds = 0);

    /**
     * @brief Структура информации о документе в RAG
     */
    struct RagDocumentInfo {
        std::string doc_id;
        std::string file_path;
        std::string file_hash;
        size_t n_tokens;
        time_t last_processed;
        bool kv_cache_available;
        std::string kv_cache_path;
    };

    /**
     * @brief Получить информацию о документе
     * @param doc_id Уникальный идентификатор документа
     * @return Информация о документе
     */
    RagDocumentInfo get_document_info(const std::string& doc_id) const;

    /**
     * @brief Попытаться загрузить KV-cache для документов из найденных чанков
     * @param chunks Найденные RAG-чанки
     * @param slot_id ID слота для загрузки KV-cache
     * @return true если хотя бы один KV-cache загружен успешно
     */
    bool try_load_document_kv_cache(const std::vector<RagChunk>& chunks, int slot_id);

    // === Информация о состоянии ===
    size_t get_external_chunks_count() const { return external_chunks_.size(); }
    size_t get_chat_history_chunks_count() const { return chat_history_chunks_.size(); }

private:
#ifdef USE_FAISS
    std::unique_ptr<faiss::Index> external_docs_index_;  // Для внешних документов
    std::unique_ptr<faiss::Index> chat_history_index_;   // Для кэширования чатов
#else
    void* external_docs_index_;  // Для внешних документов
    void* chat_history_index_;   // Для кэширования чатов
#endif
    std::vector<RagChunk> external_chunks_;
    std::vector<RagChunk> chat_history_chunks_;

    // GGUF модель для эмбеддингов
    std::unique_ptr<EmbeddingGenerator> embedding_generator_;

    // URL сервера эмбеддингов (используется прокси для форвардинга)
    std::string embedding_server_url_;

    // Модель эмбеддингов (путь к gguf), которой строится индекс
    std::string embedding_model_path_;

    // Совместимость индекса с текущей моделью эмбеддингов
    bool index_needs_reindex_ = false;
    std::string index_embedding_model_;  // Модель из metadata загруженного индекса
    std::string reindex_reason_;         // Причина несовместимости (для UI)

    // Embedding Proxy (OpenAI-compatible HTTP server)
    std::unique_ptr<EmbeddingProxy> embedding_proxy_;

    // Параметры
    int max_chunks_in_memory_ = 1000;  // Ограничение для экономии памяти
    float similarity_threshold_ = 0.7f; // Порог схожести
    int max_tokens_per_chunk_ = 256;   // Размер чанка в токенах
    static constexpr int EMBEDDING_DIMENSION = 384; // Размер эмбеддинга granite-embedding-107m
    
    // MMR параметры
    bool enable_mmr_ = false;  // Включить MMR
    float mmr_lambda_ = 0.5f;  // MMR lambda (0=релевантность, 1=разнообразие)

    // KV-cache параметры
    bool enable_kv_cache_ = true;  // Включить KV-cache для документов
    
    // Параметры гибридного поиска
    bool enable_hybrid_search_ = true;  // Включить гибридный поиск
    float keyword_boost_weight_ = 2.0f;  // Вес ключевого слова при reranking
    bool enable_query_expansion_ = true;  // Включить расширение запроса

    // KV-cache состояние: маппинг document_id → slot_id
    std::unordered_map<std::string, int> doc_id_to_slot_map_;  // document_id → slot_id

    // Кэш эмбеддингов запросов - OPTIMIZATION: unordered_map для O(1) поиска
    struct QueryEmbeddingCache {
        std::vector<float> embedding;
        uint64_t last_access;
    };
    std::unordered_map<std::string, QueryEmbeddingCache> query_embedding_cache_; // OPTIMIZATION: hash map вместо vector
    int max_embedding_cache_size_ = 100; // Размер кэша эмбеддингов (можно изменять)

    // Методы для работы с кэшем
    std::vector<float> get_cached_embedding(const std::string& text);
    void cache_embedding(const std::string& text, const std::vector<float>& embedding);
    void cleanup_embedding_cache();

    // Методы для оптимизации
    void cleanup_old_chunks();
#ifdef USE_FAISS
    std::unique_ptr<faiss::Index> create_optimized_index(int dim);
#else
    void* create_optimized_index(int dim);
#endif
    void normalize_vector(std::vector<float>& vec);
    std::string get_file_extension(const std::string& file_path);
    std::string read_txt_file(const std::string& file_path);
    void rebuild_index();

    // === Методы KV-cache ===
    std::string generate_doc_id(const std::string& file_path) const;
    std::string compute_file_hash(const std::string& file_path) const;

    // === Члены класса для KV-cache ===
    std::string kv_cache_directory_;  // Директория для хранения KV-cache
    std::shared_ptr<LlamaInterface> llama_interface_;  // Интерфейс к серверу

    // === Менеджер профилей индексов ===
    RagIndexProfileManager profile_manager_;  // Менеджер профилей

    // === Indexing Progress Tracking ===
    IndexingProgressCallback indexing_progress_callback_;
    IndexingProgress current_indexing_progress_;
    mutable std::mutex progress_mutex_;

    void report_indexing_progress(IndexingPhase phase,
                                 const std::string& file = "",
                                 int file_index = 0,
                                 int total_files = 0,
                                 int chunk = 0,
                                 int total_chunks = 0);
    void report_indexing_complete(int file_count, int chunk_count);
    void report_indexing_error(const std::string& error);
};

} // namespace core
} // namespace llama_gui