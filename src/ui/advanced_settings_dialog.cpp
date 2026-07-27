#include "../include/ui/advanced_settings_dialog.h"
#include "../include/ui/localization_manager.h"
#include "../external/imgui/imgui.h"
#include <iostream>

namespace llama_gui {
namespace ui {

AdvancedSettingsDialog::AdvancedSettingsDialog(Settings& settings)
    : settings_(settings) {}

AdvancedSettingsDialog::~AdvancedSettingsDialog() = default;

void AdvancedSettingsDialog::HelpMarker(const std::string& desc) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void AdvancedSettingsDialog::render() {
    if (!show_dialog_) return;

    // Рендерим контент внутри SettingsDialog (без собственного popup)
    // Вертикальная панель с группами слева, контент справа
    ImGui::BeginChild("GroupsPanel", ImVec2(200, -ImGui::GetFrameHeightWithSpacing() * 4), true);

    ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.4f, 1.0f), "Categories");
    ImGui::Separator();

    // GPU & Hardware
    if (ImGui::Selectable("GPU & Hardware", gpu_hardware_expanded_)) {
        gpu_hardware_expanded_ = true;
        sampling_generation_expanded_ = false;
        model_server_expanded_ = false;
        system_expanded_ = false;
    }

    // Sampling & Generation
    if (ImGui::Selectable("Sampling & Generation", sampling_generation_expanded_)) {
        gpu_hardware_expanded_ = false;
        sampling_generation_expanded_ = true;
        model_server_expanded_ = false;
        system_expanded_ = false;
    }

    // Model & Server
    if (ImGui::Selectable("Model & Server", model_server_expanded_)) {
        gpu_hardware_expanded_ = false;
        sampling_generation_expanded_ = false;
        model_server_expanded_ = true;
        system_expanded_ = false;
    }

    // System
    if (ImGui::Selectable("System", system_expanded_)) {
        gpu_hardware_expanded_ = false;
        sampling_generation_expanded_ = false;
        model_server_expanded_ = false;
        system_expanded_ = true;
    }

    ImGui::EndChild();

    ImGui::SameLine();

    // Контент в зависимости от выбранной группы
    ImGui::BeginChild("ContentPanel", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 4), true);

    if (gpu_hardware_expanded_) {
        render_gpu_hardware_group();
    } else if (sampling_generation_expanded_) {
        render_sampling_generation_group();
    } else if (model_server_expanded_) {
        render_model_server_group();
    } else if (system_expanded_) {
        render_system_group();
    }

    ImGui::EndChild();

    // Кнопки управления
    ImGui::Separator();
    if (ImGui::Button("Save")) {
        save_settings();
        hide();
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply")) {
        apply_settings();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        reset_settings();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        cancel_settings();
    }

    // Статус
    if (!status_message_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", status_message_.c_str());
    }
}

void AdvancedSettingsDialog::render_gpu_hardware_group() {
    ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.4f, 1.0f), "GPU & Hardware");
    ImGui::Separator();

    if (ImGui::BeginTabBar("GPUHardwareTabs")) {

        if (ImGui::BeginTabItem("GPU", nullptr)) {
            current_tab_ = SettingsTab::GPU;
            render_gpu_tab();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Cache", nullptr)) {
            current_tab_ = SettingsTab::Cache;
            render_cache_tab();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

void AdvancedSettingsDialog::render_sampling_generation_group() {
    ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.4f, 1.0f), "Sampling & Generation");
    ImGui::Separator();

    if (ImGui::BeginTabBar("SamplingGenerationTabs")) {

        if (ImGui::BeginTabItem("Sampling", nullptr)) {
            current_tab_ = SettingsTab::SamplingBasic;
            render_sampling_basic_tab();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Advanced", nullptr)) {
            current_tab_ = SettingsTab::SamplingAdvanced;
            render_sampling_advanced_tab();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Context", nullptr)) {
            current_tab_ = SettingsTab::Context;
            render_context_tab();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("RoPE", nullptr)) {
            current_tab_ = SettingsTab::RoPE;
            render_rope_tab();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

void AdvancedSettingsDialog::render_model_server_group() {
    ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.4f, 1.0f), "Model & Server");
    ImGui::Separator();

    if (ImGui::BeginTabBar("ModelServerTabs")) {

        if (ImGui::BeginTabItem("Model Loading", nullptr)) {
            current_tab_ = SettingsTab::ModelLoading;
            render_model_loading_tab();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Batch", nullptr)) {
            current_tab_ = SettingsTab::Batch;
            render_batch_tab();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Server Runtime", nullptr)) {
            current_tab_ = SettingsTab::ServerRuntime;
            render_server_runtime_tab();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Grammar", nullptr)) {
            current_tab_ = SettingsTab::Grammar;
            render_grammar_tab();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Control Vectors", nullptr)) {
            current_tab_ = SettingsTab::ControlVectors;
            render_control_vectors_tab();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

void AdvancedSettingsDialog::render_system_group() {
    ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.4f, 1.0f), "System");
    ImGui::Separator();

    if (ImGui::BeginTabBar("SystemTabs")) {

        if (ImGui::BeginTabItem("Logging", nullptr)) {
            current_tab_ = SettingsTab::Logging;
            render_logging_tab();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Performance", nullptr)) {
            current_tab_ = SettingsTab::Performance;
            render_performance_tab();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Advanced", nullptr)) {
            current_tab_ = SettingsTab::Advanced;
            render_advanced_tab();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Output", nullptr)) {
            current_tab_ = SettingsTab::Output;
            render_output_tab();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Tensor Override", nullptr)) {
            current_tab_ = SettingsTab::TensorOverride;
            render_tensor_override_tab();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

// =========================================================================
// Заглушки для вкладок (будут заполнены из существующих диалогов)
// =========================================================================

void AdvancedSettingsDialog::render_gpu_tab() {
    ImGui::Text("GPU Settings");
    ImGui::TextDisabled("GPU configuration moved from main settings");
    
    auto& gpu = settings_.gpu();
    
    if (ImGui::InputInt("GPU Layers", &gpu.n_gpu_layers)) {
        settings_modified_ = true;
    }
    HelpMarker("Number of layers to offload to GPU");
}

void AdvancedSettingsDialog::render_cache_tab() {
    ImGui::Text("Cache Settings");
    ImGui::TextDisabled("KV cache configuration");
    
    auto& cache = settings_.cache();
    
    ImGui::Text("Cache type K: %d", static_cast<int>(cache.cache_type_k));
    HelpMarker("Cache type for K tensors");
    
    ImGui::Text("Cache type V: %d", static_cast<int>(cache.cache_type_v));
    HelpMarker("Cache type for V tensors");
}

void AdvancedSettingsDialog::render_sampling_basic_tab() {
    ImGui::Text("Basic Sampling Settings");
    
    auto& sampling = settings_.sampling();
    
    float temp = sampling.temperature;
    if (ImGui::SliderFloat("Temperature", &temp, 0.0f, 2.0f, "%.2f")) {
        sampling.temperature = temp;
        settings_modified_ = true;
    }
}

void AdvancedSettingsDialog::render_sampling_advanced_tab() {
    ImGui::Text("Advanced Sampling Settings");
    ImGui::TextDisabled("Mirostat, penalty factors, etc.");
}

void AdvancedSettingsDialog::render_context_tab() {
    ImGui::Text("Context Settings");
    
    auto& chat = settings_.chat();
    
    if (ImGui::InputInt("Context Size", &chat.n_ctx)) {
        settings_modified_ = true;
    }
}

void AdvancedSettingsDialog::render_rope_tab() {
    ImGui::Text("RoPE Settings");
    ImGui::TextDisabled("Rotary Positional Embeddings configuration");
    
    auto& rope = settings_.rope();
    
    if (ImGui::InputFloat("RoPE Scale", &rope.rope_scale)) {
        settings_modified_ = true;
    }
    HelpMarker("RoPE scaling factor");
    
    if (ImGui::InputFloat("RoPE Freq Base", &rope.rope_freq_base)) {
        settings_modified_ = true;
    }
    HelpMarker("RoPE base frequency (0 = from model)");
    
    if (ImGui::InputFloat("RoPE Freq Scale", &rope.rope_freq_scale)) {
        settings_modified_ = true;
    }
    HelpMarker("RoPE frequency scaling factor");
}

void AdvancedSettingsDialog::render_model_loading_tab() {
    ImGui::Text("Model Loading Settings");
    ImGui::TextDisabled("Model path and adapters configuration");
    
    auto& model_loading = settings_.model_loading();
    
    static char model_path_buf[512];
    strncpy(model_path_buf, model_loading.model_path.c_str(), sizeof(model_path_buf) - 1);
    model_path_buf[sizeof(model_path_buf) - 1] = '\0';
    
    if (ImGui::InputText("Model Path", model_path_buf, sizeof(model_path_buf))) {
        model_loading.model_path = model_path_buf;
        settings_modified_ = true;
    }
}

void AdvancedSettingsDialog::render_batch_tab() {
    ImGui::Text("Batch Processing Settings");
    
    auto& batch = settings_.batch();
    
    if (ImGui::InputInt("Batch Size", &batch.batch_size)) {
        settings_modified_ = true;
    }
    HelpMarker("Batch size for processing");
    
    if (ImGui::InputInt("UBatch Size", &batch.ubatch_size)) {
        settings_modified_ = true;
    }
}

void AdvancedSettingsDialog::render_server_runtime_tab() {
    ImGui::Text("Server Runtime Settings");
    ImGui::TextDisabled("Server network and runtime configuration");

    auto& server_runtime = settings_.server_runtime();

    static char host_buf[256];
    strncpy(host_buf, server_runtime.host.c_str(), sizeof(host_buf) - 1);
    host_buf[sizeof(host_buf) - 1] = '\0';

    if (ImGui::InputText("Host", host_buf, sizeof(host_buf))) {
        server_runtime.host = host_buf;
        settings_modified_ = true;
    }

    if (ImGui::InputInt("Port", &server_runtime.port)) {
        settings_modified_ = true;
    }

    if (ImGui::InputInt("HTTP Threads", &server_runtime.threads_http)) {
        settings_modified_ = true;
    }
    HelpMarker("Number of HTTP threads (-1 = default)");

    // =========================================================================
    // KV-Cache Persistence Section
    // =========================================================================
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "KV-Cache Persistence");
    ImGui::Separator();

    // Slot save path
    {
        static char path_buf[512];
        strncpy(path_buf, server_runtime.slot_save_path.c_str(), sizeof(path_buf) - 1);
        path_buf[sizeof(path_buf) - 1] = '\0';

        if (ImGui::InputText("Slot Save Path", path_buf, sizeof(path_buf))) {
            server_runtime.slot_save_path = path_buf;
            settings_modified_ = true;
        }
        HelpMarker("Directory path for saving KV-cache slots (--slot-save-path)");
    }

    // Parallel slots
    {
        int n_parallel = server_runtime.n_parallel;
        if (ImGui::SliderInt("Parallel Slots", &n_parallel, 1, 16)) {
            server_runtime.n_parallel = n_parallel;
            settings_modified_ = true;
        }
        HelpMarker("Number of parallel processing slots (-np, --parallel)");
    }

    // K-Cache type
    {
        const char* cache_types[] = {"f32", "f16", "bf16", "q8_0", "q4_0", "q4_1", "iq4_nl"};
        int current_k_idx = 0;
        
        for (int i = 0; i < IM_ARRAYSIZE(cache_types); i++) {
            if (server_runtime.cache_type_k == cache_types[i]) {
                current_k_idx = i;
                break;
            }
        }

        if (ImGui::Combo("K-Cache Type", &current_k_idx, cache_types, IM_ARRAYSIZE(cache_types))) {
            server_runtime.cache_type_k = cache_types[current_k_idx];
            settings_modified_ = true;
        }
        HelpMarker("Data type for K cache (-ctk, --cache-type-k). q8_0 recommended.");
    }

    // V-Cache type
    {
        const char* cache_types[] = {"f32", "f16", "bf16", "q8_0", "q4_0", "q4_1", "iq4_nl"};
        int current_v_idx = 0;
        
        for (int i = 0; i < IM_ARRAYSIZE(cache_types); i++) {
            if (server_runtime.cache_type_v == cache_types[i]) {
                current_v_idx = i;
                break;
            }
        }

        if (ImGui::Combo("V-Cache Type", &current_v_idx, cache_types, IM_ARRAYSIZE(cache_types))) {
            server_runtime.cache_type_v = cache_types[current_v_idx];
            settings_modified_ = true;
        }
        HelpMarker("Data type for V cache (-ctv, --cache-type-v). q8_0 recommended.");
    }

    // Cache reuse
    {
        int cache_reuse = server_runtime.cache_reuse;
        if (ImGui::SliderInt("Cache Reuse (min tokens)", &cache_reuse, 0, 4096)) {
            server_runtime.cache_reuse = cache_reuse;
            settings_modified_ = true;
        }
        HelpMarker("Minimum chunk size for KV-cache reuse (--cache-reuse). 0 = disabled.");
    }

    // Info box
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "KV-Cache Info:");
    
    float model_size_b = 7.0f;
    int ctx_size = 4096;
    float quant_factor = 1.0f;
    if (server_runtime.cache_type_k == "q8_0" || server_runtime.cache_type_v == "q8_0") {
        quant_factor = 0.5f;
    } else if (server_runtime.cache_type_k == "q4_0" || server_runtime.cache_type_v == "q4_0") {
        quant_factor = 0.25f;
    }
    
    size_t estimated_size_mb = static_cast<size_t>(
        (model_size_b * 4.0f * quant_factor * ctx_size) / (1024.0f * 1024.0f)
    );
    
    ImGui::BulletText("Estimated KV-cache size (7B, ctx=4096): ~%zu MB per slot", estimated_size_mb);
    ImGui::BulletText("Total for %d slots: ~%zu MB", server_runtime.n_parallel, estimated_size_mb * server_runtime.n_parallel);
    ImGui::BulletText("Cache types: K=%s, V=%s", server_runtime.cache_type_k.c_str(), server_runtime.cache_type_v.c_str());
}

void AdvancedSettingsDialog::render_grammar_tab() {
    ImGui::Text("Grammar Settings");

    auto& grammar = settings_.grammar();

    static char grammar_buf[512];
    strncpy(grammar_buf, grammar.grammar_file.c_str(), sizeof(grammar_buf) - 1);
    grammar_buf[sizeof(grammar_buf) - 1] = '\0';

    if (ImGui::InputText("Grammar File", grammar_buf, sizeof(grammar_buf))) {
        grammar.grammar_file = grammar_buf;
        settings_modified_ = true;
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Reasoning Format");
    ImGui::Text("Reasoning format for models with chain-of-thought output (e.g., DeepSeek-R1)");

    // Reasoning format
    const char* reasoning_modes[] = { "None (disabled)", "Deepseek" };
    int current_mode = (grammar.reasoning_format == llama_gui::core::GrammarSettings::ReasoningFormat::Deepseek) ? 1 : 0;

    if (ImGui::Combo("Reasoning Format", &current_mode, reasoning_modes, 2)) {
        if (current_mode == 0) {
            grammar.set_reasoning_format("none");
        } else {
            grammar.set_reasoning_format("deepseek");
        }
        settings_modified_ = true;
    }
    ImGui::SameLine();
    HelpMarker("Disable reasoning to prevent model from generating thinking tokens. This significantly speeds up generation for DeepSeek-R1 models.");

    // Reasoning budget
    {
        int budget = grammar.reasoning_budget;
        if (ImGui::SliderInt("Reasoning Budget (tokens)", &budget, -1, 16384)) {
            grammar.reasoning_budget = budget;
            settings_modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Maximum tokens for reasoning (-1 = unlimited). Only used when reasoning format is enabled.");
    }

    // Display current status
    ImGui::Separator();
    if (grammar.uses_reasoning()) {
        ImGui::BulletText("Reasoning: ENABLED (%s, budget=%d)",
                          grammar.get_reasoning_format_string().c_str(),
                          grammar.reasoning_budget);
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Warning: Reasoning mode will significantly increase generation time!");
    } else {
        ImGui::BulletText("Reasoning: DISABLED (fast generation)");
    }
}

void AdvancedSettingsDialog::render_control_vectors_tab() {
    ImGui::Text("Control Vectors Settings");
    ImGui::TextDisabled("Control vectors for steering model behavior");
}

void AdvancedSettingsDialog::render_logging_tab() {
    ImGui::Text("Logging Settings");
    
    auto& perf = settings_.performance();
    
    if (ImGui::Checkbox("Enable Logging", &perf.enable_logging)) {
        settings_modified_ = true;
    }
    
    if (ImGui::Checkbox("Log to File", &perf.log_to_file)) {
        settings_modified_ = true;
    }
}

void AdvancedSettingsDialog::render_performance_tab() {
    ImGui::Text("Performance Settings");
    
    auto& perf = settings_.performance();
    
    if (ImGui::Checkbox("V-Sync", &perf.enable_vsync)) {
        settings_modified_ = true;
    }
    
    if (ImGui::SliderInt("Target FPS", &perf.target_fps, 15, 144)) {
        settings_modified_ = true;
    }
    
    if (ImGui::Checkbox("Show Performance Overlay", &perf.show_performance_overlay)) {
        settings_modified_ = true;
    }
}

void AdvancedSettingsDialog::render_advanced_tab() {
    ImGui::Text("Advanced Settings");
    ImGui::TextDisabled("Low-level llama.cpp parameters");
}

void AdvancedSettingsDialog::render_output_tab() {
    ImGui::Text("Output Settings");
    
    auto& output = settings_.output();
    
    if (ImGui::InputInt("Max Tokens", &output.n_predict)) {
        settings_modified_ = true;
    }
    HelpMarker("Maximum number of tokens to predict");
}

void AdvancedSettingsDialog::render_tensor_override_tab() {
    ImGui::Text("Tensor Override Settings");
    ImGui::TextDisabled("Override tensor split across GPUs");
}

void AdvancedSettingsDialog::save_settings() {
    std::cout << "AdvancedSettingsDialog: Saving settings" << std::endl;
    
    if (!settings_.get_current_profile_name().empty()) {
        if (settings_.save_profile(settings_.get_current_profile_name())) {
            status_message_ = "Settings saved";
        } else {
            status_message_ = "Failed to save";
        }
    }
}

void AdvancedSettingsDialog::apply_settings() {
    std::cout << "AdvancedSettingsDialog: Applying settings" << std::endl;
    status_message_ = "Settings applied";
}

void AdvancedSettingsDialog::reset_settings() {
    std::cout << "AdvancedSettingsDialog: Resetting settings" << std::endl;
    settings_.reset_to_defaults();
    status_message_ = "Settings reset to defaults";
}

void AdvancedSettingsDialog::cancel_settings() {
    hide();
}

} // namespace ui
} // namespace llama_gui
