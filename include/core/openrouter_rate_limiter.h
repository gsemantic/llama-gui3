#pragma once

#include "openrouter_types.h"
#include "openrouter_http_client.h"
#include <ctime>

namespace llama_gui {
namespace core {

/**
 * @brief Менеджер лимитов OpenRouter API
 *
 * Кэширует информацию о лимитах и проверяет доступность API.
 */
class OpenRouterRateLimiter {
public:
    explicit OpenRouterRateLimiter(OpenRouterHttpClient& http_client);

    OpenRouterRateLimit get_rate_limit();
    bool is_api_available();

private:
    OpenRouterHttpClient& http_client_;

    OpenRouterRateLimit rate_limit_cache_;
    std::time_t rate_limit_timestamp_ = 0;
    static constexpr int CACHE_DURATION_SEC = 60;
};

} // namespace core
} // namespace llama_gui
