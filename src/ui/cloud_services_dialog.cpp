#include "../include/ui/cloud_services_dialog.h"
#include "../include/core/env_manager.h"
#include "../include/ui/localization_manager.h"
#include "../include/ui/input_text_context_menu.h"
#include "../include/core/logger.h"
#include <imgui.h>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <curl/curl.h>

using namespace llama_gui::core;

namespace llama_gui {
namespace ui {

// ============================================================================
// CURL callback for model list fetch
// ============================================================================
static size_t write_string_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), total);
    return total;
}

// ============================================================================
// Provider presets
// ============================================================================
const std::vector<CloudServicesDialog::ProviderPreset>& CloudServicesDialog::get_presets() {
    static const std::vector<ProviderPreset> presets = {
        {"OpenRouter",  "https://openrouter.ai/api/v1"},
        {"OpenAI",      "https://api.openai.com/v1"},
        {"Zhipu (GLM)", "https://open.bigmodel.cn/api/paas/v4"},
        {"Together AI", "https://api.together.xyz/v1"},
        {"Groq",        "https://api.groq.com/openai/v1"},
        {"DeepInfra",   "https://api.deepinfra.com/v1/openai"},
        {"SiliconFlow", "https://api.siliconflow.com/v1"},
        {"OpenCode Zen","https://opencode.ai/zen/v1"},
        {"Custom",      ""},
    };
    return presets;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================
CloudServicesDialog::CloudServicesDialog(llama_gui::core::Settings& settings)
    : settings_(settings) {
}

CloudServicesDialog::~CloudServicesDialog() {
    models_load_cancelled_ = true;
    if (models_load_thread_.joinable()) {
        models_load_thread_.join();
    }
}

// ============================================================================
// Open / Close
// ============================================================================
void CloudServicesDialog::open() {
    is_open_ = true;
    load_from_settings();
    model_list_.clear();
    filtered_models_.clear();
    models_loaded_ = false;
    model_search_buf_[0] = '\0';
}

void CloudServicesDialog::close() {
    is_open_ = false;
    settings_modified_ = false;
}

// ============================================================================
// Load / Save settings
// ============================================================================
void CloudServicesDialog::load_from_settings() {
    auto& cp = settings_.cloud_provider();

    std::strncpy(provider_name_buf_, cp.provider_name.c_str(), sizeof(provider_name_buf_) - 1);
    provider_name_buf_[sizeof(provider_name_buf_) - 1] = '\0';

    std::strncpy(endpoint_url_buf_, cp.endpoint_url.c_str(), sizeof(endpoint_url_buf_) - 1);
    endpoint_url_buf_[sizeof(endpoint_url_buf_) - 1] = '\0';

    std::strncpy(model_id_buf_, cp.model_id.c_str(), sizeof(model_id_buf_) - 1);
    model_id_buf_[sizeof(model_id_buf_) - 1] = '\0';

    // Read API key from .env (provider-specific slot so switching providers
    // never mixes up keys; OpenCode Zen gets its own dedicated slot)
    std::string key_name = llama_gui::core::EnvManager::cloud_provider_api_key_name(cp.provider_name, cp.endpoint_url);
    std::string key = llama_gui::core::EnvManager::read_key(key_name, settings_.get_profiles_directory());
    std::strncpy(api_key_buf_, key.c_str(), sizeof(api_key_buf_) - 1);
    api_key_buf_[sizeof(api_key_buf_) - 1] = '\0';

    timeout_ms_ = cp.timeout_ms > 0 ? cp.timeout_ms : 60000;
    max_output_tokens_ = cp.max_output_tokens;
    reasoning_enabled_ = cp.reasoning_enabled;
    reasoning_budget_ = cp.reasoning_budget;
    saved_model_id_ = cp.model_id;
    settings_modified_ = false;
    show_api_key_ = false;
}

void CloudServicesDialog::save_to_settings() {
    auto& cp = settings_.cloud_provider();
    cp.provider_name = provider_name_buf_;
    cp.endpoint_url = endpoint_url_buf_;
    cp.model_id = model_id_buf_;
    auto ctx_it = model_context_map_.find(cp.model_id);
    cp.context_length = (ctx_it != model_context_map_.end()) ? ctx_it->second : 0;
    cp.timeout_ms = timeout_ms_;
    cp.max_output_tokens = max_output_tokens_ < 0 ? 0 : max_output_tokens_;
    cp.reasoning_enabled = reasoning_enabled_;
    cp.reasoning_budget = reasoning_budget_ < 0 ? 0 : reasoning_budget_;

    // Write API key to .env (never to profile JSON).
    // Key is stored per-provider so switching providers never touches other keys.
    std::string key_name = llama_gui::core::EnvManager::cloud_provider_api_key_name(provider_name_buf_, endpoint_url_buf_);
    if (api_key_buf_[0] != '\0') {
        EnvManager::write_key(key_name, api_key_buf_, settings_.get_profiles_directory());
    } else {
        EnvManager::remove_key(key_name, settings_.get_profiles_directory());
    }
}

void CloudServicesDialog::check_model_changed() {
    std::string current_model = model_id_buf_;
    if (!current_model.empty() && current_model != saved_model_id_) {
        auto& cp = settings_.cloud_provider();
        auto& recent = cp.recent_models;
        recent.erase(
            std::remove_if(recent.begin(), recent.end(),
                [&current_model](const llama_gui::core::CloudRecentModel& m) { return m.id == current_model; }),
            recent.end());
        llama_gui::core::CloudRecentModel entry;
        entry.id = current_model;
        entry.provider_name = provider_name_buf_;
        entry.endpoint_url = endpoint_url_buf_;
        recent.insert(recent.begin(), entry);
        if (recent.size() > 10) recent.resize(10);
        saved_model_id_ = current_model;
    }
}

// ============================================================================
// Fetch models from /v1/models endpoint (async)
// ============================================================================
void CloudServicesDialog::fetch_models() {
    if (models_loading_) return;

    std::string url = endpoint_url_buf_;
    std::string key = api_key_buf_;

    if (url.empty()) return;

    // Normalize URL
    if (url.back() != '/') url += '/';
    url += "models";

    models_loading_ = true;
    models_load_cancelled_ = false;
    model_list_.clear();
    filtered_models_.clear();
    model_context_map_.clear();

    // Run in background thread
    if (models_load_thread_.joinable()) {
        models_load_thread_.join();
    }

    models_load_thread_ = std::thread([this, url, key]() {
        constexpr int kMaxAttempts = 3;

        std::string response;
        CURLcode res = CURLE_OK;
        long http_code = 0;

        // CDN провайдеров (например, open.bigmodel.cn) отдают несколько A-записей,
        // часть которых может быть недостижима; порядок адресов меняется между
        // DNS-запросами, поэтому при неудаче повторяем с новым резолвом.
        for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
            CURL* curl = curl_easy_init();
            if (!curl) {
                models_loading_ = false;
                return;
            }

            response.clear();
            struct curl_slist* headers = nullptr;
            if (!key.empty()) {
                headers = curl_slist_append(headers, ("Authorization: Bearer " + key).c_str());
            }
            headers = curl_slist_append(headers, "Content-Type: application/json");

            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string_callback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms_);
            curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                             timeout_ms_ > 0 && timeout_ms_ < 15000 ? timeout_ms_ : 15000);

            res = curl_easy_perform(curl);
            http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);

            if (res == CURLE_OK && http_code == 200 && !response.empty()) {
                break;
            }
            std::cerr << "[CloudClient] Models fetch attempt " << attempt << "/" << kMaxAttempts
                      << " failed: HTTP " << http_code
                      << ", curl=" << curl_easy_strerror(res) << std::endl;
            if (attempt < kMaxAttempts && !models_load_cancelled_) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        if (res == CURLE_OK && http_code == 200 && !response.empty()) {
            try {
                auto json = nlohmann::json::parse(response);
                std::vector<std::string> models;
                std::map<std::string, int> ctx_map;

                if (json.contains("data") && json["data"].is_array()) {
                    for (const auto& item : json["data"]) {
                        if (item.contains("id") && item["id"].is_string()) {
                            std::string id = item["id"].get<std::string>();
                            models.push_back(id);
                            if (item.contains("context_length") && item["context_length"].is_number()) {
                                ctx_map[id] = item["context_length"].get<int>();
                            }
                        }
                    }
                }

                std::sort(models.begin(), models.end());

                if (!models_load_cancelled_) {
                    model_list_ = models;
                    model_context_map_ = std::move(ctx_map);
                    filter_models();
                    models_loaded_ = true;
                    std::cout << "[CloudClient] Loaded " << models.size() << " models from " << url << std::endl;
                }
            } catch (const std::exception& e) {
                std::cerr << "[CloudClient] Failed to parse model list: " << e.what() << std::endl;
            }
        } else {
            std::cerr << "[CloudClient] Failed to fetch models: HTTP " << http_code
                      << ", curl=" << curl_easy_strerror(res)
                      << ", url=" << url << std::endl;
        }

        models_loading_ = false;
    });
}

void CloudServicesDialog::filter_models() {
    filtered_models_.clear();
    std::string search = model_search_buf_;
    std::transform(search.begin(), search.end(), search.begin(), ::tolower);

    for (const auto& m : model_list_) {
        if (search.empty()) {
            filtered_models_.push_back(m);
        } else {
            std::string lower = m;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower.find(search) != std::string::npos) {
                filtered_models_.push_back(m);
            }
        }
    }
}

// ============================================================================
// Render model list
// ============================================================================
void CloudServicesDialog::render_model_list() {
    if (model_list_.empty() && !models_loading_) return;

    ImGui::Separator();
    ImGui::Text("Available models (%d):", (int)filtered_models_.size());

    // Search filter
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 200);
    ImGui::SetNextItemWidth(200);
    if (ImGui::InputTextWithHint("##model_search", "Filter...", model_search_buf_, sizeof(model_search_buf_))) {
        filter_models();
    }
    InputTextContextMenu();

    // Loading indicator
    if (models_loading_) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Loading models...");
    }

    // Model list (scrollable)
    if (ImGui::BeginChild("##model_list", ImVec2(0, 200), ImGuiChildFlags_Borders)) {
        for (const auto& model : filtered_models_) {
            bool is_selected = (model == model_id_buf_);
            if (ImGui::Selectable(model.c_str(), is_selected)) {
                std::strncpy(model_id_buf_, model.c_str(), sizeof(model_id_buf_) - 1);
                model_id_buf_[sizeof(model_id_buf_) - 1] = '\0';
                settings_modified_ = true;
            }
        }
    }
    ImGui::EndChild();
}

// ============================================================================
// Main render
// ============================================================================
void CloudServicesDialog::render() {
    if (!is_open_) return;

    ImGui::SetNextWindowSize(ImVec2(600, 0), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Cloud Provider Settings", &is_open_)) {
        auto& cp = settings_.cloud_provider();

        // Enable checkbox
        if (ImGui::Checkbox("Enable cloud provider", &cp.enabled)) {
            settings_modified_ = true;
        }

        ImGui::Separator();

        // Provider dropdown
        ImGui::Text("Provider:");
        ImGui::SameLine(120);
        {
            int current_idx = -1;
            const auto& presets = get_presets();
            for (int i = 0; i < (int)presets.size(); i++) {
                if (std::string(provider_name_buf_) == presets[i].name) {
                    current_idx = i;
                    break;
                }
            }
            if (current_idx < 0) current_idx = (int)presets.size() - 1;

            std::string items;
            for (const auto& p : presets) {
                items += p.name;
                items += '\0';
            }
            items += '\0';

            if (ImGui::Combo("##provider", &current_idx, items.c_str())) {
                if (current_idx >= 0 && current_idx < (int)presets.size()) {
                    std::strncpy(provider_name_buf_, presets[current_idx].name, sizeof(provider_name_buf_) - 1);
                    provider_name_buf_[sizeof(provider_name_buf_) - 1] = '\0';
                    if (presets[current_idx].endpoint[0] != '\0') {
                        std::strncpy(endpoint_url_buf_, presets[current_idx].endpoint, sizeof(endpoint_url_buf_) - 1);
                        endpoint_url_buf_[sizeof(endpoint_url_buf_) - 1] = '\0';
                    }
                    // Reload the API key slot for the newly selected provider
                    // so keys are never mixed up between providers
                    std::string key_name = llama_gui::core::EnvManager::cloud_provider_api_key_name(
                        provider_name_buf_, endpoint_url_buf_);
                    std::string key = llama_gui::core::EnvManager::read_key(
                        key_name, settings_.get_profiles_directory());
                    std::strncpy(api_key_buf_, key.c_str(), sizeof(api_key_buf_) - 1);
                    api_key_buf_[sizeof(api_key_buf_) - 1] = '\0';
                    // Suggest a default free model for OpenCode Zen if none set
                    if (key_name == "OPENCODE_ZEN_API_KEY" && model_id_buf_[0] == '\0') {
                        std::strncpy(model_id_buf_, "deepseek-v4-flash-free", sizeof(model_id_buf_) - 1);
                        model_id_buf_[sizeof(model_id_buf_) - 1] = '\0';
                    }
                    models_loaded_ = false; // Invalidate model list on provider change
                    settings_modified_ = true;
                }
            }
        }

        // Endpoint URL
        ImGui::Text("Endpoint:");
        ImGui::SameLine(120);
        if (ImGui::InputText("##endpoint", endpoint_url_buf_, sizeof(endpoint_url_buf_))) {
            settings_modified_ = true;
            models_loaded_ = false; // Invalidate model list on endpoint change
        }
        InputTextContextMenu();
        ImGui::SameLine();
        if (ImGui::SmallButton("Paste##ep")) {
            const char* clip = ImGui::GetClipboardText();
            if (clip) {
                std::strncpy(endpoint_url_buf_, clip, sizeof(endpoint_url_buf_) - 1);
                endpoint_url_buf_[sizeof(endpoint_url_buf_) - 1] = '\0';
                settings_modified_ = true;
                models_loaded_ = false;
            }
        }

        // API Key
        ImGui::Text("API Key:");
        ImGui::SameLine(120);
        {
            ImGuiInputTextFlags flags = show_api_key_ ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_Password;
            if (ImGui::InputText("##api_key", api_key_buf_, sizeof(api_key_buf_), flags)) {
                settings_modified_ = true;
                models_loaded_ = false; // Invalidate model list on key change
            }
            InputTextContextMenu();
            ImGui::SameLine();
            if (ImGui::SmallButton(show_api_key_ ? "Hide" : "Show")) {
                show_api_key_ = !show_api_key_;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Paste")) {
                const char* clip = ImGui::GetClipboardText();
                if (clip) {
                    std::strncpy(api_key_buf_, clip, sizeof(api_key_buf_) - 1);
                    api_key_buf_[sizeof(api_key_buf_) - 1] = '\0';
                    settings_modified_ = true;
                    models_loaded_ = false;
                }
            }
        }

        // Model ID
        ImGui::Text("Model:");
        ImGui::SameLine(120);
        if (ImGui::InputText("##model_id", model_id_buf_, sizeof(model_id_buf_))) {
            settings_modified_ = true;
        }
        InputTextContextMenu();
        ImGui::SameLine();
        if (ImGui::SmallButton("Paste##model")) {
            const char* clip = ImGui::GetClipboardText();
            if (clip) {
                std::strncpy(model_id_buf_, clip, sizeof(model_id_buf_) - 1);
                model_id_buf_[sizeof(model_id_buf_) - 1] = '\0';
                settings_modified_ = true;
            }
        }

        // Load models button
        ImGui::SameLine();
        if (ImGui::SmallButton("Load Models") && !models_loading_) {
            fetch_models();
        }

        // Timeout
        ImGui::Text("Timeout (ms):");
        ImGui::SameLine(120);
        if (ImGui::InputInt("##timeout", &timeout_ms_, 1000, 5000)) {
            if (timeout_ms_ < 1000) timeout_ms_ = 1000;
            settings_modified_ = true;
        }

        // Max output tokens (0 = unlimited)
        ImGui::Text("Max output tokens:");
        ImGui::SameLine(120);
        if (ImGui::InputInt("##max_output_tokens", &max_output_tokens_, 256, 2048)) {
            if (max_output_tokens_ < 0) max_output_tokens_ = 0;
            settings_modified_ = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled(max_output_tokens_ == 0 ? "(unlimited)" : "");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("0 = no limit (the model/provider decides its own maximum)");
        }

        // Reasoning / thinking mode (для поддерживающих моделей: GLM, DeepSeek, o-серия)
        ImGui::Separator();
        if (ImGui::Checkbox("Enable reasoning / thinking##reasoning_enabled", &reasoning_enabled_)) {
            settings_modified_ = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Enable chain-of-thought for models that support it (GLM, DeepSeek, OpenAI o-series).\n"
                              "When off, thinking is disabled (required for GLM-4.7 to return content).");
        }

        if (ImGui::SliderInt("Reasoning budget##reasoning_budget", &reasoning_budget_, 0, 16384,
                             reasoning_budget_ == 0 ? "provider default" : "%d")) {
            if (reasoning_budget_ < 0) reasoning_budget_ = 0;
            settings_modified_ = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Max tokens spent on reasoning (0 = provider/model default).\n"
                              "Only used when thinking is enabled.");
        }

        // Model list (if loaded)
        if (models_loaded_ || models_loading_) {
            render_model_list();
        }

        ImGui::Separator();

        // Recent models
        if (!cp.recent_models.empty()) {
            ImGui::Text("Recent models:");
            for (size_t i = 0; i < cp.recent_models.size(); i++) {
                const auto& rm = cp.recent_models[i];
                bool is_selected = (rm.id == model_id_buf_);
                if (ImGui::RadioButton(rm.id.c_str(), is_selected)) {
                    std::strncpy(model_id_buf_, rm.id.c_str(), sizeof(model_id_buf_) - 1);
                    model_id_buf_[sizeof(model_id_buf_) - 1] = '\0';
                    if (!rm.provider_name.empty()) {
                        std::strncpy(provider_name_buf_, rm.provider_name.c_str(), sizeof(provider_name_buf_) - 1);
                        provider_name_buf_[sizeof(provider_name_buf_) - 1] = '\0';
                        if (!rm.endpoint_url.empty()) {
                            std::strncpy(endpoint_url_buf_, rm.endpoint_url.c_str(), sizeof(endpoint_url_buf_) - 1);
                            endpoint_url_buf_[sizeof(endpoint_url_buf_) - 1] = '\0';
                        }
                        // Ключ хранится по слоту провайдера — подтягиваем его
                        std::string key_name = llama_gui::core::EnvManager::cloud_provider_api_key_name(
                            provider_name_buf_, endpoint_url_buf_);
                        std::string saved_key = llama_gui::core::EnvManager::read_key(
                            key_name, settings_.get_profiles_directory());
                        std::strncpy(api_key_buf_, saved_key.c_str(), sizeof(api_key_buf_) - 1);
                        api_key_buf_[sizeof(api_key_buf_) - 1] = '\0';
                    }
                    settings_modified_ = true;
                }
                ImGui::SameLine();
                std::string label = "X##del_" + std::to_string(i);
                if (ImGui::SmallButton(label.c_str())) {
                    cp.recent_models.erase(cp.recent_models.begin() + i);
                    if (is_selected) {
                        model_id_buf_[0] = '\0';
                    }
                    settings_modified_ = true;
                    break;
                }
            }
            ImGui::Separator();
        }

        // Status (провайдер активен только при наличии ключа и выбранной модели)
        bool has_key = (api_key_buf_[0] != '\0');
        if (cp.enabled && has_key && model_id_buf_[0] != '\0') {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Cloud provider: active (%s)", model_id_buf_);
        } else if (cp.enabled) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Cloud provider: configured but incomplete");
        } else {
            ImGui::Text("Using local server");
        }

        ImGui::Separator();

        // Buttons
        if (ImGui::Button("Apply")) {
            save_to_settings();
            check_model_changed();
            settings_modified_ = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Save")) {
            save_to_settings();
            check_model_changed();
            std::string profile = settings_.get_current_profile_name();
            if (!profile.empty()) {
                settings_.save_profile(profile);
            }
            settings_modified_ = false;
            is_open_ = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            is_open_ = false;
            settings_modified_ = false;
        }
    }
    ImGui::End();
}

} // namespace ui
} // namespace llama_gui
