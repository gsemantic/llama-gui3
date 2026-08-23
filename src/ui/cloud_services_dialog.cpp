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
        // requires_key=false — публичные эндпоинты, проверены и работают без API-ключа
        {"Zhipu (GLM)",      "https://open.bigmodel.cn/api/paas/v4",           true},
        {"OpenCode Zen",     "https://opencode.ai/zen/v1",                     false},
        {"Pollinations",     "https://text.pollinations.ai/openai",            false},
        {"OVH AI Endpoints", "https://oai.endpoints.kepler.ai.cloud.ovh.net/v1", false},
        {"Custom",           "",                                               true},
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
    check_cancelled_ = true;
    if (check_thread_.joinable()) {
        check_thread_.join();
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
    check_cancelled_ = true;
    if (check_thread_.joinable()) {
        check_thread_.join();
    }
    checking_ = false;
    model_check_map_.clear();
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
    std::string key = read_provider_key(cp.provider_name, cp.endpoint_url);
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

    // Обновляем/создаём запись недавней модели с ТЕКУЩИМ провайдером —
    // так старые записи без привязки (мигрированные из строк) получают её
    // после первого же Apply.
    if (!cp.model_id.empty()) {
        auto& recent = cp.recent_models;
        auto r_it = std::find_if(recent.begin(), recent.end(),
            [&cp](const llama_gui::core::CloudRecentModel& r) { return r.id == cp.model_id; });
        if (r_it != recent.end()) {
            r_it->provider_name = cp.provider_name;
            r_it->endpoint_url = cp.endpoint_url;
        } else {
            llama_gui::core::CloudRecentModel entry;
            entry.id = cp.model_id;
            entry.provider_name = cp.provider_name;
            entry.endpoint_url = cp.endpoint_url;
            recent.insert(recent.begin(), entry);
            if (recent.size() > 10) recent.resize(10);
        }
    }

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
                            // Пропускаем заведомо не-чатовые модели (embeddings,
                            // tts, whisper, image-gen, guard-модели и т.п.)
                            std::string lower = id;
                            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                            bool not_chat =
                                lower.find("whisper") != std::string::npos ||
                                lower.find("-tts") != std::string::npos ||
                                lower.find("tts-") != std::string::npos ||
                                lower.find("embedding") != std::string::npos ||
                                lower.find("bge-") != std::string::npos ||
                                lower.find("rerank") != std::string::npos ||
                                lower.find("moderation") != std::string::npos ||
                                lower.find("guard") != std::string::npos ||
                                lower.find("diffusion") != std::string::npos ||
                                lower.find("-xl-base") != std::string::npos;
                            if (not_chat) continue;
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

// ============================================================================
// Availability check: probe each listed model with a minimal chat request
// ============================================================================
std::string CloudServicesDialog::build_chat_url(const std::string& endpoint_url) {
    std::string url = endpoint_url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += "/chat/completions";
    return url;
}

std::string CloudServicesDialog::read_provider_key(const std::string& provider_name,
                                                   const std::string& endpoint_url) {
    const std::string dir = settings_.get_profiles_directory();
    const std::string key_name = EnvManager::cloud_provider_api_key_name(provider_name, endpoint_url);
    std::string key = EnvManager::read_key(key_name, dir);

    // Разовая миграция: до появления отдельных слотов ключ Zhipu (GLM) жил в
    // общем CLOUD_PROVIDER_API_KEY. Переносим его в персональный слот.
    if (key.empty() && key_name == "ZHIPU_GLM_API_KEY") {
        std::string legacy = EnvManager::read_key("CLOUD_PROVIDER_API_KEY", dir);
        if (!legacy.empty()) {
            EnvManager::write_key(key_name, legacy, dir);
            key = legacy;
            std::cout << "[CloudProvider] Migrated legacy CLOUD_PROVIDER_API_KEY -> "
                      << key_name << std::endl;
        }
    }
    return key;
}

void CloudServicesDialog::start_model_checks() {
    if (checking_ || model_list_.empty()) return;

    check_cancelled_ = true;
    if (check_thread_.joinable()) {
        check_thread_.join();
    }
    check_cancelled_ = false;

    {
        std::lock_guard<std::mutex> lk(model_check_mutex_);
        model_check_map_.clear();
        for (const auto& m : model_list_) {
            model_check_map_[m] = ModelCheckResult{};
        }
    }

    const std::string url = build_chat_url(endpoint_url_buf_);
    const std::string key = api_key_buf_;
    const int timeout = timeout_ms_ > 0 ? timeout_ms_ : 60000;
    const std::vector<std::string> models = model_list_;

    checking_ = true;
    check_thread_ = std::thread([this, url, key, timeout, models]() {
        constexpr int kProbeMaxTokens = 8;
        constexpr int kPauseMs = 700;

        auto pause = [&]() {
            for (int waited = 0; waited < kPauseMs && !check_cancelled_; waited += 100) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        };

        for (const auto& model : models) {
            if (check_cancelled_) break;

            {
                std::lock_guard<std::mutex> lk(model_check_mutex_);
                model_check_map_[model].state = ModelCheckResult::Checking;
            }

            nlohmann::json body;
            body["model"] = model;
            body["messages"] = nlohmann::json::array();
            body["messages"].push_back({{"role", "user"}, {"content", "Reply with the single word OK"}});
            body["max_tokens"] = kProbeMaxTokens;
            body["stream"] = false;
            // ВАЖНО: CURLOPT_POSTFIELDS не копирует данные — строка обязана
            // жить до завершения curl_easy_perform.
            const std::string body_str = body.dump();

            // Выполняет POST; with_auth=false — анонимная попытка без ключа
            auto perform_probe = [&](bool with_auth, std::string& resp, long& code, CURLcode& cres) {
                resp.clear();
                CURL* curl = curl_easy_init();
                if (!curl) {
                    cres = CURLE_FAILED_INIT;
                    code = 0;
                    return;
                }
                struct curl_slist* headers = nullptr;
                headers = curl_slist_append(headers, "Content-Type: application/json");
                if (with_auth && !key.empty()) {
                    headers = curl_slist_append(headers, ("Authorization: Bearer " + key).c_str());
                }
                curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                curl_easy_setopt(curl, CURLOPT_POST, 1L);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string_callback);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
                curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout);
                curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                                 timeout > 0 && timeout < 15000 ? timeout : 15000L);
                curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
                curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

                cres = curl_easy_perform(curl);
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
            };

            std::cout << "[CloudCheck] model=" << model << " key="
                      << (key.empty() ? "none" : ("len " + std::to_string(key.size())))
                      << std::endl;

            std::string response;
            long http_code = 0;
            CURLcode res = CURLE_OK;
            auto t0 = std::chrono::steady_clock::now();
            perform_probe(true, response, http_code, res);

            // Сохранённый ключ может быть протухшим: публичные шлюзы пускают
            // анонимно, поэтому при 401 повторяем попытку без Authorization.
            // Если анонимная попытка удачна — модель доступна и без ключа.
            bool retried_anonymously = false;
            if (res == CURLE_OK && http_code == 401 && !key.empty()) {
                std::cout << "[CloudCheck] 401 with stored key, retrying anonymously" << std::endl;
                perform_probe(false, response, http_code, res);
                retried_anonymously = true;
            }
            long latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();

            ModelCheckResult r;
            r.latency_ms = latency_ms;
            bool alive = false;
            if (res == CURLE_OK && http_code == 200 && !response.empty()) {
                try {
                    auto j = nlohmann::json::parse(response);
                    // Reasoning-модели могут отдать пустой content при обрезанном
                    // бюджете токенов — главное, что choices пришли: модель жива.
                    alive = j.contains("choices") && j["choices"].is_array() && !j["choices"].empty();
                } catch (const std::exception&) {
                    alive = false;
                }
            }
            if (alive) {
                r.state = ModelCheckResult::Ok;
                // [free]: модель ответила без ключа — либо слот пуст, либо
                // сохранённый ключ был отклонён и помог анонимный запрос.
                r.anonymous_ok = retried_anonymously || key.empty();
            } else {
                r.state = ModelCheckResult::Fail;
                r.http_code = (res == CURLE_OK) ? http_code : -1;
                if (res != CURLE_OK) {
                    r.error = curl_easy_strerror(res);
                } else if (http_code == 429) {
                    r.error = "rate limited";
                } else if (http_code == 400 &&
                           response.find("navailable") != std::string::npos) {
                    r.error = "model unavailable upstream";
                } else if (http_code == 401 || http_code == 403) {
                    r.error = "auth/quota/region";
                } else if (http_code == 404) {
                    r.error = "model not supported";
                } else if (!response.empty()) {
                    r.error = response.substr(0, 120);
                } else {
                    r.error = "HTTP " + std::to_string(http_code);
                }
            }

            {
                std::lock_guard<std::mutex> lk(model_check_mutex_);
                model_check_map_[model] = r;
            }

            pause();
        }
        checking_ = false;
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

    int total_checked = 0;
    int alive_count = 0;
    {
        std::lock_guard<std::mutex> lk(model_check_mutex_);
        for (const auto& kv : model_check_map_) {
            if (kv.second.state == ModelCheckResult::Ok || kv.second.state == ModelCheckResult::Fail) {
                ++total_checked;
                if (kv.second.state == ModelCheckResult::Ok) ++alive_count;
            }
        }
    }

    ImGui::Separator();
    ImGui::Text("Available models (%d):", (int)filtered_models_.size());
    if (total_checked > 0) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "checked %d/%d, alive %d",
                           total_checked, (int)model_list_.size(), alive_count);
    }

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
        const float status_x = ImGui::GetWindowWidth() - 175;
        for (const auto& model : filtered_models_) {
            bool is_selected = (model == model_id_buf_);
            if (ImGui::Selectable(model.c_str(), is_selected)) {
                std::strncpy(model_id_buf_, model.c_str(), sizeof(model_id_buf_) - 1);
                model_id_buf_[sizeof(model_id_buf_) - 1] = '\0';
                settings_modified_ = true;
            }
            bool row_hovered = ImGui::IsItemHovered();

            ModelCheckResult r;
            {
                std::lock_guard<std::mutex> lk(model_check_mutex_);
                auto it = model_check_map_.find(model);
                if (it != model_check_map_.end()) r = it->second;
            }
            if (r.state == ModelCheckResult::None) continue;

            ImGui::SameLine(status_x);
            char label[64];
            switch (r.state) {
                case ModelCheckResult::Checking:
                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "checking...");
                    break;
                case ModelCheckResult::Ok:
                    if (r.anonymous_ok) {
                        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "OK %ld ms [free]", r.latency_ms);
                    } else {
                        ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "OK %ld ms [key]", r.latency_ms);
                    }
                    break;
                case ModelCheckResult::Fail:
                    if (r.http_code > 0) {
                        snprintf(label, sizeof(label), "HTTP %ld", r.http_code);
                    } else {
                        snprintf(label, sizeof(label), "%.20s", r.error.c_str());
                    }
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", label);
                    break;
                default:
                    break;
            }
            if (row_hovered && (r.state == ModelCheckResult::Ok || !r.error.empty())) {
                if (r.state == ModelCheckResult::Ok) {
                    ImGui::SetTooltip("%s", r.anonymous_ok
                        ? "Модель отвечает без API-ключа (публичный доступ)"
                        : "Модель работает с вашим сохранённым ключом");
                } else {
                    ImGui::SetTooltip("%s", r.error.c_str());
                }
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
                    std::string key = read_provider_key(provider_name_buf_, endpoint_url_buf_);
                    std::strncpy(api_key_buf_, key.c_str(), sizeof(api_key_buf_) - 1);
                    api_key_buf_[sizeof(api_key_buf_) - 1] = '\0';
                    // Suggest a default free model for OpenCode Zen if none set
                    if (key_name == "OPENCODE_ZEN_API_KEY" && model_id_buf_[0] == '\0') {
                        std::strncpy(model_id_buf_, "deepseek-v4-flash-free", sizeof(model_id_buf_) - 1);
                        model_id_buf_[sizeof(model_id_buf_) - 1] = '\0';
                    }
                    if (key_name == "POLLINATIONS_API_KEY" && model_id_buf_[0] == '\0') {
                        std::strncpy(model_id_buf_, "openai-fast", sizeof(model_id_buf_) - 1);
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

        // Probe every listed model with a minimal chat request (sequential,
        // with pauses — public endpoints enforce strict per-model rate limits)
        ImGui::SameLine();
        if (ImGui::SmallButton(checking_ ? "Checking..." : "Check Models") && !checking_ && models_loaded_) {
            start_model_checks();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Send a tiny test request to each model and show\n"
                              "which ones actually answer (latency / HTTP error).\n"
                              "\"Load Models\" first. Sequential to respect rate limits.");
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
                std::string label = rm.id;
                if (rm.provider_name.empty()) {
                    label += " (?)";
                }
                bool is_selected = (rm.id == model_id_buf_);
                if (ImGui::RadioButton(label.c_str(), is_selected)) {
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
                        std::string saved_key = read_provider_key(rm.provider_name, rm.endpoint_url);
                        std::strncpy(api_key_buf_, saved_key.c_str(), sizeof(api_key_buf_) - 1);
                        api_key_buf_[sizeof(api_key_buf_) - 1] = '\0';
                    }
                    settings_modified_ = true;
                }
                ImGui::SameLine();
                std::string del_label = "X##del_" + std::to_string(i);
                if (ImGui::SmallButton(del_label.c_str())) {
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

        // Status (провайдер активен при наличии ключа — либо когда ключ не нужен)
        bool preset_requires_key = true;
        for (const auto& p : get_presets()) {
            if (std::string(provider_name_buf_) == p.name) {
                preset_requires_key = p.requires_key;
                break;
            }
        }
        bool has_credentials = (api_key_buf_[0] != '\0') || !preset_requires_key;
        if (cp.enabled && has_credentials && model_id_buf_[0] != '\0') {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Cloud provider: active (%s)%s",
                               model_id_buf_, preset_requires_key ? "" : " [public endpoint]");
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
