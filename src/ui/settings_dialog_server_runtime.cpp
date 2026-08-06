#include "../include/ui/settings_dialog_server_runtime.h"
#include "../include/ui/localization_manager.h"
#include "../external/imgui/imgui.h"
#include <iostream>
#include <sstream>

namespace llama_gui {
namespace ui {

ServerRuntimeDialog::ServerRuntimeDialog(Settings& settings)
    : settings_(settings) {
    new_api_key_[0] = '\0';
}

ServerRuntimeDialog::~ServerRuntimeDialog() = default;

void ServerRuntimeDialog::HelpMarker(const std::string& desc) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void ServerRuntimeDialog::render_api_key_editor(size_t index) {
    auto& runtime = settings_.server_runtime();
    auto& keys = runtime.api_keys;

    ImGui::PushID(static_cast<int>(index));

    char key_buf[256];
    strncpy(key_buf, keys[index].c_str(), sizeof(key_buf) - 1);
    key_buf[sizeof(key_buf) - 1] = '\0';

    ImGui::SetNextItemWidth(250);
    if (ImGui::InputText("##api_key", key_buf, sizeof(key_buf), ImGuiInputTextFlags_Password)) {
        keys[index] = key_buf;
        modified_ = true;
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("Remove##remove_key")) {
        runtime.remove_api_key(index);
        modified_ = true;
        ImGui::PopID();
        return;
    }

    ImGui::PopID();
}

void ServerRuntimeDialog::render_network_section() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Network Settings");
    ImGui::Separator();

    auto& runtime = settings_.server_runtime();

    // Host
    {
        char buf[128];
        strncpy(buf, runtime.host.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        if (ImGui::InputText("Host##host", buf, sizeof(buf))) {
            runtime.host = buf;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Host to bind server to");
    }

    // Port
    {
        int port = runtime.port;
        if (ImGui::SliderInt("Port##port", &port, 1, 65535)) {
            runtime.port = port;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Port to bind server to");
    }

    // Timeout
    {
        int timeout = runtime.timeout;
        if (ImGui::SliderInt("Timeout (seconds)##timeout", &timeout, 0, 3600)) {
            runtime.timeout = timeout;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Request timeout in seconds");
    }

    ImGui::Separator();

    // API Prefix
    {
        char buf[256];
        strncpy(buf, runtime.api_prefix.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        if (ImGui::InputText("API Prefix##api_prefix", buf, sizeof(buf))) {
            runtime.api_prefix = buf;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("URL prefix for API endpoints");
    }

    // Offline mode
    {
        bool offline = runtime.offline;
        if (ImGui::Checkbox("Offline Mode##offline", &offline)) {
            runtime.offline = offline;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Disable external connections");
    }

    ImGui::Separator();

    // HTTP Threads
    {
        int threads = runtime.threads_http;
        if (ImGui::SliderInt("HTTP Threads##http_threads", &threads, -1, 32)) {
            runtime.threads_http = threads;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Number of HTTP threads (-1 = auto)");
    }

    // Static path
    {
        char buf[512];
        strncpy(buf, runtime.static_path.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        if (ImGui::InputText("Static Files Path##static_path", buf, sizeof(buf))) {
            runtime.static_path = buf;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Path to serve static files from");
    }

    // No WebUI
    {
        bool no_webui = runtime.no_webui;
        if (ImGui::Checkbox("Disable WebUI##no_webui", &no_webui)) {
            runtime.no_webui = no_webui;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Disable built-in web UI");
    }
}

void ServerRuntimeDialog::render_security_section() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Security");
    ImGui::Separator();

    auto& runtime = settings_.server_runtime();

    // API Keys
    ImGui::Text("API Keys:");
    ImGui::SameLine();
    HelpMarker("Keys required for API access");

    auto& keys = runtime.api_keys;
    for (size_t i = 0; i < keys.size(); ++i) {
        render_api_key_editor(i);
    }

    // Add new API key
    ImGui::SetNextItemWidth(250);
    ImGui::InputText("New API Key##new_api_key", new_api_key_, sizeof(new_api_key_), ImGuiInputTextFlags_Password);

    ImGui::SameLine();
    if (ImGui::Button("Add##add_api_key")) {
        if (new_api_key_[0] != '\0') {
            runtime.add_api_key(new_api_key_);
            modified_ = true;
            new_api_key_[0] = '\0';
        }
    }

    ImGui::Separator();

    // API Key File
    {
        char buf[512];
        strncpy(buf, runtime.api_key_file.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        if (ImGui::InputText("API Key File##api_key_file", buf, sizeof(buf))) {
            runtime.api_key_file = buf;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("File containing API keys (one per line)");

        if (ImGui::Button("Browse##api_key_file")) {
            // TODO: Open file dialog
        }
    }

    ImGui::Separator();

    // SSL Key File
    {
        char buf[512];
        strncpy(buf, runtime.ssl_key_file.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        if (ImGui::InputText("SSL Key File##ssl_key", buf, sizeof(buf))) {
            runtime.ssl_key_file = buf;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Path to SSL private key file");

        if (ImGui::Button("Browse##ssl_key")) {
            // TODO: Open file dialog
        }
    }

    // SSL Cert File
    {
        char buf[512];
        strncpy(buf, runtime.ssl_cert_file.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        if (ImGui::InputText("SSL Cert File##ssl_cert", buf, sizeof(buf))) {
            runtime.ssl_cert_file = buf;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Path to SSL certificate file");

        if (ImGui::Button("Browse##ssl_cert")) {
            // TODO: Open file dialog
        }
    }

    // SSL status
    ImGui::Separator();
    if (runtime.has_ssl_config()) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "SSL: Configured");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "SSL: Not configured");
    }
}

void ServerRuntimeDialog::render_features_section() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Features");
    ImGui::Separator();

    auto& runtime = settings_.server_runtime();

    // Embeddings mode
    {
        bool embeddings = runtime.embeddings_mode;
        if (ImGui::Checkbox("Embeddings Mode##embeddings", &embeddings)) {
            runtime.embeddings_mode = embeddings;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Enable embeddings generation endpoint");
    }

    // Reranking mode
    {
        bool reranking = runtime.reranking_mode;
        if (ImGui::Checkbox("Reranking Mode##reranking", &reranking)) {
            runtime.reranking_mode = reranking;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Enable reranking endpoint");
    }

    // Metrics
    {
        bool metrics = runtime.metrics_enabled;
        if (ImGui::Checkbox("Enable Metrics##metrics", &metrics)) {
            runtime.metrics_enabled = metrics;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Enable Prometheus metrics endpoint");
    }
}

void ServerRuntimeDialog::render_kv_cache_section() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", TR("settings.kv_cache.title"));
    ImGui::Separator();

    auto& runtime = settings_.server_runtime();

    // Slot save path
    {
        char buf[512];
        strncpy(buf, runtime.slot_save_path.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        if (ImGui::InputText(TR("settings.kv_cache.slot_save_path"), buf, sizeof(buf))) {
            runtime.slot_save_path = buf;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("settings.kv_cache.slot_save_path.tooltip"));
    }

    // Parallel slots
    {
        int n_parallel = runtime.n_parallel;
        if (ImGui::SliderInt(TR("settings.kv_cache.parallel_slots"), &n_parallel, 1, 16)) {
            runtime.n_parallel = n_parallel;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("settings.kv_cache.parallel_slots.tooltip"));
    }

    // K-Cache type
    {
        const char* cache_types[] = {"f32", "f16", "bf16", "q8_0", "q4_0", "q4_1", "iq4_nl"};
        int current_k_idx = 0;
        
        for (int i = 0; i < IM_ARRAYSIZE(cache_types); i++) {
            if (runtime.cache_type_k == cache_types[i]) {
                current_k_idx = i;
                break;
            }
        }

        if (ImGui::Combo(TR("settings.kv_cache.k_cache_type"), &current_k_idx, cache_types, IM_ARRAYSIZE(cache_types))) {
            runtime.cache_type_k = cache_types[current_k_idx];
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("settings.kv_cache.k_cache_type.tooltip"));
    }

    // V-Cache type
    {
        const char* cache_types[] = {"f32", "f16", "bf16", "q8_0", "q4_0", "q4_1", "iq4_nl"};
        int current_v_idx = 0;
        
        for (int i = 0; i < IM_ARRAYSIZE(cache_types); i++) {
            if (runtime.cache_type_v == cache_types[i]) {
                current_v_idx = i;
                break;
            }
        }

        if (ImGui::Combo(TR("settings.kv_cache.v_cache_type"), &current_v_idx, cache_types, IM_ARRAYSIZE(cache_types))) {
            runtime.cache_type_v = cache_types[current_v_idx];
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("settings.kv_cache.v_cache_type.tooltip"));
    }

    // Cache reuse
    {
        int cache_reuse = runtime.cache_reuse;
        if (ImGui::SliderInt(TR("settings.kv_cache.cache_reuse"), &cache_reuse, 0, 4096)) {
            runtime.cache_reuse = cache_reuse;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("settings.kv_cache.cache_reuse.tooltip"));
    }

    // Info box
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "%s", TR("settings.kv_cache.info.title"));
    
    // Estimate size for 7B model
    float model_size_b = 7.0f;
    int ctx_size = 4096;
    
    // Simple estimation based on cache type
    float quant_factor = 1.0f;
    if (runtime.cache_type_k == "q8_0" || runtime.cache_type_v == "q8_0") {
        quant_factor = 0.5f;
    } else if (runtime.cache_type_k == "q4_0" || runtime.cache_type_v == "q4_0") {
        quant_factor = 0.25f;
    }
    
    size_t estimated_size_mb = static_cast<size_t>(
        (model_size_b * 4.0f * quant_factor * ctx_size) / (1024.0f * 1024.0f)
    );
    
    ImGui::BulletText(TR("settings.kv_cache.info.estimated_size"), estimated_size_mb);
    ImGui::BulletText(TR("settings.kv_cache.info.total_size"), runtime.n_parallel, estimated_size_mb * runtime.n_parallel);
    ImGui::BulletText(TR("settings.kv_cache.info.cache_types"), runtime.cache_type_k.c_str(), runtime.cache_type_v.c_str());
}

void ServerRuntimeDialog::render() {
    // =========================================================================
    // Network Section
    // =========================================================================
    render_network_section();

    ImGui::Separator();

    // =========================================================================
    // Security Section
    // =========================================================================
    render_security_section();

    ImGui::Separator();

    // =========================================================================
    // Features Section
    // =========================================================================
    render_features_section();

    ImGui::Separator();

    // =========================================================================
    // KV-Cache Persistence Section
    // =========================================================================
    render_kv_cache_section();

    // =========================================================================
    // Summary
    // =========================================================================
    ImGui::Separator();
    ImGui::Text("Configuration Summary:");

    auto& runtime = settings_.server_runtime();

    ImGui::BulletText("Server: %s:%d", runtime.host.c_str(), runtime.port);
    ImGui::BulletText("Timeout: %d seconds", runtime.timeout);
    ImGui::BulletText("API Keys: %zu configured", runtime.api_keys.size());
    ImGui::BulletText("SSL: %s", runtime.has_ssl_config() ? "Yes" : "No");
    ImGui::BulletText("Features:%s%s%s",
                      runtime.embeddings_mode ? " Embeddings" : "",
                      runtime.reranking_mode ? " Reranking" : "",
                      runtime.metrics_enabled ? " Metrics" : "");
    ImGui::BulletText("KV-Cache: %s (%d slots, %s)",
                      runtime.slot_save_path.empty() ? TRF("settings.kv_cache.info.not_configured", "Не настроено") : TRF("settings.kv_cache.info.configured", "Настроено"),
                      runtime.n_parallel,
                      runtime.cache_type_k.c_str());
}

} // namespace ui
} // namespace llama_gui
