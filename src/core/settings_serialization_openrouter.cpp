#include "../include/core/settings.h"
#include "../include/core/env_manager.h"
#include <nlohmann/json.hpp>
#include <iostream>

namespace llama_gui {
namespace core {

using json = nlohmann::json;

void Settings::serializeOpenRouterSettings(json& j) const {
    // Serialize as "cloud_provider" (new format)
    // NOTE: api_key is NOT serialized here — it lives in .env
    j["cloud_provider"] = {
        {"enabled", openrouter_settings_.enabled},
        {"provider_name", openrouter_settings_.provider_name},
        {"endpoint_url", openrouter_settings_.endpoint_url},
        {"model_id", openrouter_settings_.model_id},
        {"timeout_ms", openrouter_settings_.timeout_ms},
        {"free_models_only", openrouter_settings_.free_models_only},
        {"last_search_query", openrouter_settings_.last_search_query},
        {"recent_models", openrouter_settings_.recent_models},
        {"usage_total_requests", openrouter_settings_.usage_total_requests},
        {"usage_remaining", openrouter_settings_.usage_remaining},
        {"usage_limit", openrouter_settings_.usage_limit}
    };
}

void Settings::deserializeOpenRouterSettings(const json& j) {
    // Try new format first: "cloud_provider"
    if (j.contains("cloud_provider")) {
        auto& o = j["cloud_provider"];
        openrouter_settings_.enabled = o.value("enabled", false);
        openrouter_settings_.provider_name = o.value("provider_name", "");
        openrouter_settings_.endpoint_url = o.value("endpoint_url", "");
        openrouter_settings_.model_id = o.value("model_id", "");
        openrouter_settings_.timeout_ms = o.value("timeout_ms", 30000);
        openrouter_settings_.free_models_only = o.value("free_models_only", false);
        openrouter_settings_.last_search_query = o.value("last_search_query", "");
        if (o.contains("recent_models") && o["recent_models"].is_array()) {
            openrouter_settings_.recent_models = o["recent_models"].get<std::vector<std::string>>();
        }
        openrouter_settings_.usage_total_requests = o.value("usage_total_requests", 0);
        openrouter_settings_.usage_remaining = o.value("usage_remaining", 50);
        openrouter_settings_.usage_limit = o.value("usage_limit", 50);
        return;
    }

    // Fallback: legacy "openrouter" format — migrate to new format
    if (j.contains("openrouter")) {
        auto& o = j["openrouter"];
        openrouter_settings_.enabled = o.value("enabled", false);
        openrouter_settings_.model_id = o.value("selected_model", "");
        openrouter_settings_.endpoint_url = o.value("custom_base_url", "");
        openrouter_settings_.provider_name = "OpenRouter";
        openrouter_settings_.timeout_ms = o.value("timeout_ms", 30000);
        openrouter_settings_.free_models_only = o.value("free_models_only", false);
        openrouter_settings_.last_search_query = o.value("last_search_query", "");
        if (o.contains("recent_models") && o["recent_models"].is_array()) {
            openrouter_settings_.recent_models = o["recent_models"].get<std::vector<std::string>>();
        }
        openrouter_settings_.usage_total_requests = o.value("usage_total_requests", 0);
        openrouter_settings_.usage_remaining = o.value("usage_remaining", 50);
        openrouter_settings_.usage_limit = o.value("usage_limit", 50);

        // Migrate: move legacy api_key to .env
        std::string legacy_key = o.value("api_key", "");
        if (!legacy_key.empty()) {
            std::cout << "[Migration] Moving legacy openrouter api_key to .env" << std::endl;
            EnvManager::write_key("CLOUD_PROVIDER_API_KEY", legacy_key, profiles_directory_);
            // NOTE: api_key stays in old JSON until next profile save overwrites with new format
        }

        // Set default endpoint for OpenRouter if empty
        if (openrouter_settings_.endpoint_url.empty()) {
            openrouter_settings_.endpoint_url = "https://openrouter.ai/api/v1";
        }

        std::cout << "[Migration] Migrated openrouter -> cloud_provider" << std::endl;
    }
}

} // namespace core
} // namespace llama_gui
