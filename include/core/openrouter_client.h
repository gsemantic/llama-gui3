#pragma once

#include "openrouter_types.h"
#include "openrouter_http_client.h"
#include "openrouter_model_parser.h"
#include "openrouter_rate_limiter.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace llama_gui {
namespace core {

/**
 * @brief Клиент для работы с OpenRouter API (фасад)
 *
 * API документация: https://openrouter.ai/docs
 *
 * Делегирует HTTP, парсинг и лимиты вспомогательным классам.
 */
class OpenRouterClient {
public:
    using ModelsCallback = std::function<void(const OpenRouterModelsResponse& response)>;
    using CompletionCallback = std::function<void(const OpenRouterCompletionResponse& response)>;
    using StreamCallback = std::function<void(const std::string& token, bool is_done)>;

    explicit OpenRouterClient(const std::string& api_key = "");
    ~OpenRouterClient();

    OpenRouterClient(const OpenRouterClient&) = delete;
    OpenRouterClient& operator=(const OpenRouterClient&) = delete;

    void set_api_key(const std::string& api_key);
    std::string get_api_key() const { return http_client_.get_api_key(); }

    void set_base_url(const std::string& url);
    std::string get_base_url() const { return http_client_.get_base_url(); }

    void set_timeout(int timeout_ms);

    void set_proxy(const std::string& proxy_url);

    /// Прервать активную стриминг-генерацию (по требованию пользователя).
    void abort_stream();

    // Модели
    bool get_models_async(ModelsCallback callback);
    OpenRouterModelsResponse get_models();

    bool get_free_models_async(ModelsCallback callback);
    OpenRouterModelsResponse get_free_models();

    bool search_models_async(const std::string& query, bool free_only, ModelsCallback callback);
    OpenRouterModelsResponse search_models(const std::string& query, bool free_only = false);

    OpenRouterModelDetails get_model_details(const std::string& model_id);

    // Генерация текста
    OpenRouterCompletionResponse complete(const OpenRouterRequestParams& params);
    bool complete_streaming_async(const OpenRouterRequestParams& params, StreamCallback callback,
                                 OpenRouterCompletionResponse* out_response = nullptr);

    // API
    bool is_api_available();
    OpenRouterRateLimit get_rate_limit();

private:
    OpenRouterHttpClient http_client_;
    OpenRouterModelParser model_parser_;
    OpenRouterRateLimiter rate_limiter_;
    std::atomic<bool> aborted_{false};
};

} // namespace core
} // namespace llama_gui
