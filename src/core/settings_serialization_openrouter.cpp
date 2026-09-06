#include "../include/core/settings.h"
#include "../include/core/env_manager.h"
#include <nlohmann/json.hpp>
#include <iostream>

namespace llama_gui {
namespace core {

using json = nlohmann::json;

namespace {

// recent_models хранятся объектами {id, provider_name, endpoint_url};
// строки из старого формата мигрируют без привязки к провайдеру.
void load_recent_models(const json& o, std::vector<CloudRecentModel>& out) {
    out.clear();
    if (!o.contains("recent_models") || !o["recent_models"].is_array()) {
        return;
    }
    for (const auto& item : o["recent_models"]) {
        CloudRecentModel r;
        if (item.is_string()) {
            r.id = item.get<std::string>();
        } else if (item.is_object()) {
            r.id = item.value("id", "");
            r.provider_name = item.value("provider_name", "");
            r.endpoint_url = item.value("endpoint_url", "");
        } else {
            continue;
        }
        if (!r.id.empty()) {
            out.push_back(std::move(r));
        }
    }
}

} // namespace

void Settings::serializeOpenRouterSettings(json& j) const {
    // Serialize as "cloud_provider" (new format)
    // NOTE: api_key is NOT serialized here — it lives in .env
    j["cloud_provider"] = {
        {"enabled", openrouter_settings_.enabled},
        {"provider_name", openrouter_settings_.provider_name},
        {"endpoint_url", openrouter_settings_.endpoint_url},
        {"model_id", openrouter_settings_.model_id},
        {"context_length", openrouter_settings_.context_length},
        {"timeout_ms", openrouter_settings_.timeout_ms},
        {"max_output_tokens", openrouter_settings_.max_output_tokens},
        {"reasoning_enabled", openrouter_settings_.reasoning_enabled},
        {"reasoning_budget", openrouter_settings_.reasoning_budget},
        {"free_models_only", openrouter_settings_.free_models_only},
        {"last_search_query", openrouter_settings_.last_search_query},
        {"auto_price", openrouter_settings_.auto_price},
        {"price_input_per_1m", openrouter_settings_.price_input_per_1m},
        {"price_output_per_1m", openrouter_settings_.price_output_per_1m},
        {"use_tor", openrouter_settings_.use_tor},
        {"socks5_proxy_host", openrouter_settings_.socks5_proxy_host}
    };
    json recent = json::array();
    for (const auto& r : openrouter_settings_.recent_models) {
        recent.push_back({
            {"id", r.id},
            {"provider_name", r.provider_name},
            {"endpoint_url", r.endpoint_url}
        });
    }
    j["cloud_provider"]["recent_models"] = recent;
}

void Settings::deserializeOpenRouterSettings(const json& j) {
    // Try new format first: "cloud_provider"
    if (j.contains("cloud_provider")) {
        auto& o = j["cloud_provider"];
        openrouter_settings_.enabled = o.value("enabled", false);
        openrouter_settings_.provider_name = o.value("provider_name", "");
        openrouter_settings_.endpoint_url = o.value("endpoint_url", "");
        openrouter_settings_.model_id = o.value("model_id", "");
        openrouter_settings_.context_length = o.value("context_length", 0);
        openrouter_settings_.timeout_ms = o.value("timeout_ms", 30000);
        openrouter_settings_.max_output_tokens = o.value("max_output_tokens", 0);
        openrouter_settings_.reasoning_enabled = o.value("reasoning_enabled", false);
        openrouter_settings_.reasoning_budget = o.value("reasoning_budget", 0);
        openrouter_settings_.free_models_only = o.value("free_models_only", false);
        openrouter_settings_.last_search_query = o.value("last_search_query", "");
        openrouter_settings_.auto_price = o.value("auto_price", true);
        openrouter_settings_.price_input_per_1m = o.value("price_input_per_1m", 0.0);
        openrouter_settings_.price_output_per_1m = o.value("price_output_per_1m", 0.0);
        openrouter_settings_.use_tor = o.value("use_tor", false);
        openrouter_settings_.socks5_proxy_host = o.value("socks5_proxy_host", "127.0.0.1:9050");
        load_recent_models(o, openrouter_settings_.recent_models);
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
        openrouter_settings_.max_output_tokens = o.value("max_output_tokens", 0);
        openrouter_settings_.reasoning_enabled = o.value("reasoning_enabled", false);
        openrouter_settings_.reasoning_budget = o.value("reasoning_budget", 0);
        openrouter_settings_.free_models_only = o.value("free_models_only", false);
        openrouter_settings_.last_search_query = o.value("last_search_query", "");
        load_recent_models(o, openrouter_settings_.recent_models);

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
