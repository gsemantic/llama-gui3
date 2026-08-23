#pragma once

#include <string>
#include <vector>
#include <functional>
#include <curl/curl.h>

namespace llama_gui {
namespace core {

/**
 * @brief HTTP клиент для OpenRouter API
 *
 * Инкапсулирует логику HTTP запросов через CURL.
 */
class OpenRouterHttpClient {
public:
    /**
     * @brief Колбэк стримингового запроса
     *
     * @param data       данные SSE-чанка (JSON) или тело ошибки
     * @param is_error   true, если получена ошибка (HTTP-статус != 200 или CURL-ошибка)
     * @param is_done    true, когда поток завершён
     * @param retryable  true, если ошибка транзиентная и имеет смысл повторить запрос
     */
    using StreamCallback = std::function<void(const std::string& data, bool is_error, bool is_done, bool retryable)>;

    OpenRouterHttpClient() = default;
    ~OpenRouterHttpClient();

    OpenRouterHttpClient(const OpenRouterHttpClient&) = delete;
    OpenRouterHttpClient& operator=(const OpenRouterHttpClient&) = delete;

    void set_base_url(const std::string& url);
    void set_api_key(const std::string& api_key);
    void set_timeout(int timeout_ms);

    std::string get_base_url() const { return base_url_; }
    std::string get_api_key() const { return api_key_; }

    std::string make_request(const std::string& endpoint, const std::string& body = "");

    /// HTTP-код последнего ответа (0, если запрос не дошёл до сервера)
    long last_http_code() const { return last_http_code_; }

    /**
     * @brief Синхронный стриминговый запрос (SSE).
     *
     * Разбирает SSE-поток и вызывает колбэк по мере поступления чанков.
     * Завершается на маркере [DONE] или при закрытии соединения сервером.
     *
     * @param connect_timeout_ms таймаут установки соединения для этой попытки
     */
    bool make_streaming_request(const std::string& endpoint, const std::string& body,
                                const StreamCallback& callback, int connect_timeout_ms = 30000);

private:
    struct StreamContext {
        StreamCallback callback;
        std::string buffer;
        bool done = false;
    };

    std::string base_url_ = "https://openrouter.ai/api/v1";
    std::string api_key_;
    int timeout_ms_ = 30000;
    long last_http_code_ = 0;

    std::string build_url(const std::string& endpoint);
    std::vector<std::string> get_request_headers() const;

    static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp);
    static size_t stream_write_callback(void* contents, size_t size, size_t nmemb, void* userp);
};

} // namespace core
} // namespace llama_gui
