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
 * @brief Нативный вызов инструмента в раскладке OpenAI
 *
 * arguments — СЫРОЙ JSON: провайдер присылает его по кускам и на
 * последнем может быть неполным. Разбирать его здесь нельзя — единственный
 * разбор аргументов у потребителя.
 */
struct ToolCall {
    std::string id;          /* провайдер может не прислать — остаётся индекс */
    std::string type = "function";
    std::string name;
    std::string arguments;
};

/**
 * @brief Сообщение в чате
 */
struct ChatMessage {
    MessageRole role;
    std::string content;
    /*
     * Нативный вызов инструмента (И6.2). Пусто, пока провайдер не умеет
     * tools; тогда вызов приходит текстом и разбирает плагин.
     */
    std::vector<ToolCall> tool_calls;

    ChatMessage() = default;
    ChatMessage(MessageRole r, const std::string& c) : role(r), content(c) {}
};

/**
 * @brief Один инструмент, объявленный серверу (И6.2)
 *
 * Раскладка OpenAI, потому что её понимает llama-server с --jinja
 * (в b7472 --jinja включён по умолчанию). parameters — JSON Schema:
 * {"type":"object","properties":{...},"required":[...]}. Схема инструмента
 * и есть контракт: сервер отдаёт аргументы, которые не обязаны соответствовать
 * задумке, если схема расплывчата.
 */
struct ToolSpec {
    std::string type = "function";
    std::string name;
    std::string description;
    json parameters;
};

/**
 * @brief Чем ограничен ответ (И6.2)
 *
 * Пустой type = не ограничивать, и тогда в тело запроса поле не уходит
 * вовсе. Иначе сервер без json_object отвечает 400, и это должно быть
 * видно, а не молчаливый возврат к тексту.
 */
struct ResponseFormat {
    std::string type;        /* "", "text", "json_object" */
    json schema;             /* для {"type":"json_schema", ...} */
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

    /*
     * И6.2. Пустой tools — поля в тело запроса не уходят, и поведение
     * ровно прежнее: сервер ничего не узнаёт и не может отказать.
     */
    std::vector<ToolSpec> tools;
    /* "", "auto", "none", "required" или {"type":"function","name":...} */
    std::string tool_choice;
    ResponseFormat response_format;

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

    /*
     * И6.2. Пока локальный клиент отбрасывал код ответа, отказ сервера был
     * неотличим от пустого ответа: choices пуст, error пуст, и вызывающий
     * решал, что модель просто ничего не сказала. На этом и строилась бы
     * неверная догадка «tools не поддерживаются».
     *
     * ok=false означает, что запрос НЕ выполнен: error содержит причину,
     * http_code — её код. ok=true при пустом choices — модель ответила, и
     * ответ действительно пуст.
     */
    bool ok = true;
    long http_code = 0;
    std::string error;
};

/**
 * @brief Что сервер умеет из OpenAI-совместимых полей
 *
 * Три состояния, а не два, потому что «не знаю» и «не умеет» ведут себя
 * по-разному: второе выключает нативный вызов навсегда, первое — только
 * до следующей попытки. Свести их — значит одним сетевым сбоем навсегда
 * отключить tools.
 */
enum class ToolsSupport {
    Unknown = 0,   /* не проверяли или не смогли проверить */
    Supported,     /* сервер принял запрос с tools */
    Rejected       /* сервер отказал именно из-за tools */
};

/*
 * Решение по ответу сервера о поддержке tools — ЧИСТАЯ функция.
 *
 * Почему по ответу, а не по версии: llama-server сообщает об отсутствии
 * поддержки явно («tools param requires --jinja flag», «Invalid tool_choice»)
 * кодом 400, а версию не сообщает вовсе. Проверять «по версии» значило бы
 * хардкодить знание о сборке сервера, которое протухает молча.
 *
 * Ключевое правило: сетевой сбой (http_code == 0) — это Unknown, а НЕ
 * Rejected. Иначе один неудачный запрос навсегда выключил бы нативный
 * вызов, и агент поехал бы на текстовом протоколе, не сказав об этом.
 */
ToolsSupport classify_tools_response(long http_code, const std::string& body);

/*
 * Тело POST /v1/chat/completions — единственное место, где собирается
 * запрос. Два вызова (потоковый и блокирующий) раньше собирали тело
 * отдельно и разошлись: в потоковый уходили min_p/repeat_penalty/top_k,
 * в блокирующий — нет, то есть один и тот же ход агента набирался с
 * разным сэмплированием в зависимости от пути.
 *
 * Поле уходит в тело, только если отличается от нейтрального значения.
 * Это не экономия, а способ сказать «здесь нужны настройки сервера» явно:
 * блокирующий путь плагина выставляет min_p=0 и repeat_penalty=1, и поле
 * молча пропадает из запроса вместо того, чтобы ехать нулём и обрезать
 * низковероятные токены у агента, пишущего код.
 */
json build_chat_body(const ChatCompletionRequest& request, bool stream);

/*
 * Нативные вызовы из ответа провайдера. Аргументы остаются СЫРЫМ JSON:
 * провайдер шлёт их по кускам и на последнем может быть неполным, а
 * разбор аргументов принадлежит потребителю (плагин валидирует по схеме
 * инструмента, core/tool.h).
 *
 * Вызов без имени отбрасывается: исполнить его нечем, и пропущенный молча
 * оставил бы в ответе пустой вызов, который цикл счёл бы выполненным.
 *
 * Провайдер нумерует вызовы индексом, а не строкой (находка И5.1): у
 * вызова без id остаётся индекс в name-free виде, поэтому порядок
 * сохраняется по порядку в массиве, а не по идентификатору.
 */
std::vector<ToolCall> parse_tool_calls(const json& array);

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
 * @brief Отмена одного потока (И6.6)
 *
 * Флаг, а не «всё сразу»: stop_streaming_requests() рвёт ВСЕ потоки хоста,
 * и плагин, отменяя свой ход, не должен останавливать ещё и чат приложения.
 *
 * Проверяется в колбэках записи и прогресса, поэтому отмена прерывает
 * передачу по-настоящему: локальный сервер теряет клиента и освобождает
 * слот. «Перестать читать» без этого оставляло бы генерацию впустую.
 */
class StreamCancel {
public:
    void request() { flag_.store(true, std::memory_order_release); }
    bool requested() const { return flag_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> flag_{false};
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
    /* cancel — необязателен; без него поток нельзя отменить по требованию. */
    void create_chat_completion_streaming(const ChatCompletionRequest& request,
                                          StreamCallback callback,
                                          StreamCancel* cancel = nullptr);
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
