#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <future>
#include <nlohmann/json.hpp>
#include <curl/curl.h>

namespace llama_gui {
namespace core {

using json = nlohmann::json;

// Forward declarations
namespace impl {
    struct SlotOperationResult;
    class LlamaInterfaceImpl;
}

using json = nlohmann::json;

/**
 * @brief Типы сообщений в чате
 */
enum class MessageRole {
    User,
    Assistant,
    System
};

/**
 * @brief Сообщение в чате
 */
struct ChatMessage {
    MessageRole role;
    std::string content;
    
    ChatMessage() = default;
    ChatMessage(MessageRole r, const std::string& c) : role(r), content(c) {}
};

/**
 * @brief Запрос на создание чата
 */
struct ChatCompletionRequest {
    std::string model;
    std::vector<ChatMessage> messages;

    // Generation params
    int max_tokens = 512;
    float temperature = 0.7f;
    float top_p = 0.9f;
    int top_k = 40;
    float min_p = 0.05f;
    float repeat_penalty = 1.1f;
    float presence_penalty = 0.0f;
    float frequency_penalty = 0.0f;

    int mirostat_mode = 0;
    float mirostat_tau = 5.0f;
    float mirostat_eta = 0.1f;

    bool stop_on_newline = false;
    bool stream = false;

    // Additional llama.cpp parameters
    int threads = 4; // CPU threads
    int n_ctx = 4096; // Context size
    int seed = -1; // Random seed (-1 for random)
    float tfs_z = 1.0f; // Tail free sampling (1.0 = disabled)
    float typical_p = 1.0f; // Typical sampling (1.0 = disabled)
    int n_gpu_layers = 0; // GPU layers
    std::string tensor_split = ""; // GPU tensor split
    bool mlock = false; // Lock memory
    bool no_mmap = false; // Disable memory mapping
    std::string numa = "none"; // NUMA strategy
    std::vector<std::string> lora_adapters; // LoRA adapters
    std::string lora_base = ""; // Base model for LoRA
    std::string mmproj = ""; // Multimodal projector
    std::string grammar = ""; // Grammar file
    std::string chat_template = ""; // Chat template
    bool embedding = false; // Embedding mode
    std::vector<std::string> reverse_prompt; // Reverse prompts (stop sequences)
    std::string log_format = "text"; // Log format
    int verbosity = 0; // Verbosity level

    // Additional params
    std::vector<std::string> stop;
    std::vector<std::pair<std::string, float>> logit_bias;
};

/**
 * @brief Ответ с информацией о чате
 */
struct ChatCompletionResponse {
    std::string id;
    std::string object;
    int64_t created;
    std::string model;
    
    struct ChatChoice {
        int index;
        ChatMessage message;
        std::string finish_reason;
    };
    
    std::vector<ChatChoice> choices;
    json usage;
};

/**
 * @brief Запрос на эмбеддинг
 */
struct EmbeddingRequest {
    std::string model;
    std::string input;
};

/**
 * @brief Ответ с эмбеддингом
 */
struct EmbeddingResponse {
    std::string object;
    
    struct EmbeddingData {
        int index;
        std::vector<float> embedding;
    };
    
    std::vector<EmbeddingData> data;
    json usage;
};

/**
 * @brief Статус подключения к серверу
 */
enum class ConnectionStatus {
    Disconnected,
    Connecting,
    Connected,
    Error,
    Timeout
};

/**
 * @brief Типы запросов к серверу
 */
enum class RequestType {
    ChatCompletion,
    Completion,
    Embedding,
    Tokenization,
    ModelInfo,
    Health
};

/**
 * @brief Статус выполнения запроса
 */
enum class RequestStatus {
    Pending,
    Processing,
    Completed,
    Failed,
    Cancelled
};

/**
 * @brief Информация о модели
 */
struct ModelInfo {
    std::string id;
    std::string name;
    std::string description;
    int context_length = 0;
    int max_tokens = 0;
    bool supports_streaming = false;
    bool supports_functions = false;
    std::vector<std::string> parameters;
};

/**
 * @brief Результат запроса
 */
struct RequestResult {
    RequestStatus status;
    std::string content;
    std::string error_message;
    int tokens_generated = 0;
    double processing_time = 0.0; // в секундах
    std::string request_id;
};

/**
 * @brief Параметры запроса
 */
struct RequestParams {
    std::string model;
    std::string prompt;
    std::string system_prompt;
    int max_tokens = 512;
    float temperature = 0.7f;
    float top_p = 0.9f;
    float repeat_penalty = 1.1f;
    bool stream = true;
    std::vector<std::string> stop;
    std::vector<std::pair<std::string, float>> logit_bias;
};

/**
 * @brief Интерфейс для взаимодействия с llama.cpp сервером
 */
class LlamaInterface {
public:
    using StreamCallback = std::function<void(const std::string& chunk, bool is_final)>;

public:
    explicit LlamaInterface(const std::string& server_url = "http://localhost:8081");
    ~LlamaInterface();

    // Запрет копирования
    LlamaInterface(const LlamaInterface&) = delete;
    LlamaInterface& operator=(const LlamaInterface&) = delete;

    // Инициализация
    bool initialize(const std::string& server_url);

    // Основные методы
    void create_chat_completion_streaming(const ChatCompletionRequest& request, StreamCallback callback);
    std::future<ChatCompletionResponse> create_chat_completion_async(const ChatCompletionRequest& request);
    EmbeddingResponse create_embedding(const EmbeddingRequest& request);
    
    // Информация о сервере
    bool is_server_healthy() const;
    json get_server_info() const;
    json get_models() const;
    json get_slots_status() const;

    // =========================================================================
    // KV-cache Slot Management (управление слотами KV-cache)
    // =========================================================================

    /**
     * @brief Структура результата операции с слотом
     */

    /**
     * @brief Сохранить KV-cache указанного слота в файл
     * @param slot_id ID слота (0..n_parallel-1)
     * @param filename Имя файла для сохранения
     * @return true если успешно
     */
    bool save_slot_kv_cache(int slot_id, const std::string& filename);

    /**
     * @brief Восстановить KV-cache указанного слота из файла
     * @param slot_id ID слота (0..n_parallel-1)
     * @param filename Имя файла для восстановления
     * @return true если успешно
     */
    bool restore_slot_kv_cache(int slot_id, const std::string& filename);

    /**
     * @brief Сохранить KV-cache с подробным результатом
     * @param slot_id ID слота
     * @param filename Имя файла
     * @return impl::SlotOperationResult с деталями операции
     */
    impl::SlotOperationResult save_slot_kv_cache_detailed(int slot_id, const std::string& filename);

    /**
     * @brief Восстановить KV-cache с подробным результатом
     * @param slot_id ID слота
     * @param filename Имя файла
     * @return impl::SlotOperationResult с деталями операции
     */
    impl::SlotOperationResult restore_slot_kv_cache_detailed(int slot_id, const std::string& filename);

    /**
     * @brief Сбросить слот
     * @param slot_id ID слота
     * @return true если успешно
     */
    bool reset_slot(int slot_id);

    /**
     * @brief Удалить KV-cache слота
     * @param slot_id ID слота
     * @return true если успешно
     */
    bool erase_slot(int slot_id);

    /**
     * @brief Загрузить текст в слот для токенизации и сохранения KV-cache
     * @param slot_id ID слота
     * @param text Текст для токенизации
     * @return impl::SlotOperationResult с деталями операции
     */
    impl::SlotOperationResult tokenize_text_in_slot(int slot_id, const std::string& text);

    // Настройки
    void set_api_key(const std::string& api_key);
    void set_ssl_verify(bool verify);
    void set_timeout(int seconds);

    /**
     * @brief Получить статус всех слотов
     * @return JSON со статусом слотов
     */
    bool parse_streaming_response(const std::string& response, StreamCallback callback);
    
    // Внутренний метод для HTTP запросов
    std::string make_http_request(const std::string& endpoint, const std::string& method, const json& data) const;

    // Асинхронные HTTP методы с callback'ами
    using HttpResponseCallback = std::function<void(const std::string& response)>;
    void make_async_http_request(const std::string& endpoint, const std::string& method, const json& data, HttpResponseCallback callback);
    
    // Стриминговый HTTP запрос
    void make_streaming_http_request(const std::string& endpoint, const std::string& method, const json& data, StreamCallback callback);

    // Остановка всех streaming запросов
    void stop_streaming_requests();

    // Асинхронная версия create_chat_completion с callback
    using ChatCompletionCallback = std::function<void(const ChatCompletionResponse& response)>;
    void create_chat_completion_async_callback(const ChatCompletionRequest& request, ChatCompletionCallback callback);

    // Утилита для валидации и очистки UTF-8
    static std::string validate_and_clean_utf8(const std::string& input);

    // Утилита для извлечения JSON из ответа
    static json extract_json_from_response(const std::string& response);

    // Метод для обработки асинхронных запросов
    void process_async_requests();

private:
    std::unique_ptr<impl::LlamaInterfaceImpl> pImpl;

    std::string server_url_;
    std::string api_key_;
    int timeout_seconds_;
    std::string last_error_;
};

} // namespace core
} // namespace llama_gui
