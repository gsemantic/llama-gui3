#include "../../include/core/openrouter_rate_limiter.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <ctime>

using json = nlohmann::json;

namespace llama_gui {
namespace core {

OpenRouterRateLimiter::OpenRouterRateLimiter(OpenRouterHttpClient& http_client)
    : http_client_(http_client) {
}

OpenRouterRateLimit OpenRouterRateLimiter::get_rate_limit() {
    std::time_t now = std::time(nullptr);
    if (rate_limit_timestamp_ > 0 &&
        (now - rate_limit_timestamp_) < CACHE_DURATION_SEC &&
        rate_limit_cache_.remaining_requests > 0) {
        return rate_limit_cache_;
    }

    std::string response = http_client_.make_request("auth/key");

    if (!response.empty()) {
        try {
            json data = json::parse(response);

            if (data.contains("data")) {
                const auto& key_data = data["data"];

                if (key_data.contains("usage")) {
                    const auto& usage = key_data["usage"];
                    if (usage.contains("requests")) {
                        rate_limit_cache_.total_requests = usage.value("requests", 0);
                    }
                }

                if (key_data.contains("limit")) {
                    rate_limit_cache_.limit = key_data.value("limit", 50);
                }

                if (key_data.contains("is_free")) {
                    rate_limit_cache_.is_free_tier = key_data.value("is_free", true);
                }

                rate_limit_cache_.remaining_requests =
                    std::max(0, rate_limit_cache_.limit - rate_limit_cache_.total_requests);
            }
        } catch (...) {
            // Ошибка парсинга - возвращаем кэш
        }
    }

    rate_limit_timestamp_ = now;
    return rate_limit_cache_;
}

bool OpenRouterRateLimiter::is_api_available() {
    std::string response = http_client_.make_request("models");
    return !response.empty();
}

} // namespace core
} // namespace llama_gui
