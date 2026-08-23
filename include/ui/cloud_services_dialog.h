#pragma once

#include "../core/openrouter_types.h"
#include "../core/openrouter_client.h"
#include "../core/settings.h"
#include <imgui.h>
#include <vector>
#include <string>
#include <map>
#include <memory>
#include <atomic>
#include <thread>

namespace llama_gui {
namespace ui {

/**
 * @brief Cloud provider settings dialog (single page, no tabs)
 *
 * Configures any OpenAI-compatible endpoint:
 * - Provider name, endpoint URL, API key (stored in .env), model ID
 * - Fetches model list from /v1/models endpoint
 * - API key is read/written to .env file, never stored in profile JSON
 */
class CloudServicesDialog {
public:
    explicit CloudServicesDialog(llama_gui::core::Settings& settings);
    ~CloudServicesDialog();

    void open();
    void close();
    bool is_open() const { return is_open_; }
    void render();

private:
    llama_gui::core::Settings& settings_;

    bool is_open_ = false;
    bool settings_modified_ = false;

    // Form buffers (copied from settings on open)
    char provider_name_buf_[256] = "";
    char endpoint_url_buf_[512] = "";
    char api_key_buf_[512] = "";
    char model_id_buf_[256] = "";
    int timeout_ms_ = 60000;
    int max_output_tokens_ = 0;   // 0 = не ограничено
    bool reasoning_enabled_ = false;  // Режим размышлений/thinking
    int reasoning_budget_ = 0;       // Бюджет токенов на reasoning (0 = по умолчанию)
    bool show_api_key_ = false;

    // Saved state for change detection
    std::string saved_model_id_;

    // Model list
    std::vector<std::string> model_list_;
    std::vector<std::string> filtered_models_;
    std::map<std::string, int> model_context_map_;
    char model_search_buf_[256] = "";
    bool models_loaded_ = false;
    bool models_loading_ = false;
    std::thread models_load_thread_;
    std::atomic<bool> models_load_cancelled_{false};

    void load_from_settings();
    void save_to_settings();
    void check_model_changed();

    // Model list
    void fetch_models();
    void filter_models();
    void render_model_list();

    // Provider presets
    struct ProviderPreset {
        const char* name;
        const char* endpoint;
    };
    static const std::vector<ProviderPreset>& get_presets();
};

} // namespace ui
} // namespace llama_gui
