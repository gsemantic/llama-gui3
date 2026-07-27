#pragma once

#include <string>
#include <vector>
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

private:
    std::string base_url_ = "https://openrouter.ai/api/v1";
    std::string api_key_;
    int timeout_ms_ = 30000;

    std::string build_url(const std::string& endpoint);
    std::vector<std::string> get_request_headers() const;

    static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp);
};

} // namespace core
} // namespace llama_gui
