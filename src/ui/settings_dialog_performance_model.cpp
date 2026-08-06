#include "../include/ui/settings_dialog.h"
#include "../include/ui/settings_dialog_model.h"
#include "../include/ui/localization_manager.h"
#include "../include/core/logger.h"
#include "../external/imgui/imgui.h"
#include <cstring>
#include <iostream>

namespace llama_gui {
namespace ui {

void SettingsDialog::render_performance_settings() {
    auto& perf = settings_.performance();

    ImGui::Text(TR("performance.settings"));
    ImGui::Separator();

    // Debug Mode checkbox
    if (ImGui::Checkbox(TR("performance.debug_mode"), &perf.debug_mode)) {
        // Apply logging mode immediately
        llama_gui::core::Logger::instance().set_debug_mode(perf.debug_mode);
    }
    ImGui::SameLine();
    HelpMarker(TR("performance.debug_mode.help"));

    ImGui::Separator();
    ImGui::Text(TR("performance.display_rendering"));
    ImGui::Separator();

    // VSync
    if (ImGui::Checkbox(TR("performance.enable_vsync"), &perf.enable_vsync)) {
        // VSync setting would be applied to renderer
    }
    ImGui::SameLine();
    HelpMarker(TR("performance.enable_vsync.help"));

    // Target FPS
    int target_fps = perf.target_fps;
    if (ImGui::SliderInt(TR("performance.target_fps"), &target_fps, 10, 144)) {
        perf.target_fps = target_fps;
    }
    ImGui::SameLine();
    HelpMarker(TR("performance.target_fps.help"));

    // Smart Redraw
    if (ImGui::Checkbox(TR("performance.smart_redraw"), &perf.enable_smart_redraw)) {
        // Smart redraw setting
    }
    ImGui::SameLine();
    HelpMarker(TR("performance.smart_redraw.help"));

    ImGui::Separator();
    ImGui::Text(TR("performance.logging"));
    ImGui::Separator();

    // Enable logging
    if (ImGui::Checkbox(TR("performance.enable_logging"), &perf.enable_logging)) {
        // Logging setting
    }

    // Log level
    const char* log_levels[] = { "None", "Error", "Warning", "Info", "Debug" };
    int current_log_level = 0;
    if (perf.log_level == "Error") current_log_level = 1;
    else if (perf.log_level == "Warning") current_log_level = 2;
    else if (perf.log_level == "Info") current_log_level = 3;
    else if (perf.log_level == "Debug") current_log_level = 4;

    if (ImGui::Combo(TR("performance.log_level"), &current_log_level, log_levels, 5)) {
        switch (current_log_level) {
            case 0: perf.log_level = "None"; break;
            case 1: perf.log_level = "Error"; break;
            case 2: perf.log_level = "Warning"; break;
            case 3: perf.log_level = "Info"; break;
            case 4: perf.log_level = "Debug"; break;
        }
    }
    ImGui::SameLine();
    HelpMarker(TR("performance.log_level.help"));

    // Log to file
    if (ImGui::Checkbox(TR("performance.log_to_file"), &perf.log_to_file)) {
        // Log to file setting
    }
    ImGui::SameLine();
    if (perf.log_to_file) {
        ImGui::SameLine();
        char log_path_buffer[512];
        strncpy(log_path_buffer, perf.log_file_path.c_str(), sizeof(log_path_buffer) - 1);
        if (ImGui::InputText("##LogPath", log_path_buffer, sizeof(log_path_buffer))) {
            perf.log_file_path = log_path_buffer;
        }
    }
}

void SettingsDialog::render_model_settings() {
    // Delegate to model settings dialog
    if (model_settings_dialog_) {
        model_settings_dialog_->render();
    }
}

void SettingsDialog::render_advanced_settings() {
    auto& chat_settings = settings_.chat();

    ImGui::Text(TR("performance.llama_params"));
    ImGui::Separator();

    ImGui::Text(TR("performance.system_resources"));
    ImGui::Separator();

    // CPU Threads
    ImGui::InputInt(TR("performance.cpu_threads"), &chat_settings.threads);
    ImGui::SameLine();
    HelpMarker(TR("performance.cpu_threads.help"));

    // Context Size
    ImGui::InputInt(TR("performance.context_size"), &chat_settings.n_ctx);
    ImGui::SameLine();
    HelpMarker(TR("performance.context_size.help"));

    // Random Seed
    ImGui::InputInt(TR("performance.seed"), &chat_settings.seed);
    ImGui::SameLine();
    HelpMarker(TR("performance.seed.help"));

    ImGui::Separator();
    ImGui::Text(TR("performance.sampling"));

    // Tail Free Sampling
    ImGui::SliderFloat(TR("performance.tail_free"), &chat_settings.tfs_z, 0.0f, 1.0f);
    ImGui::SameLine();
    HelpMarker(TR("performance.tail_free.help"));

    // Typical Sampling
    ImGui::SliderFloat(TR("performance.typical"), &chat_settings.typical_p, 0.0f, 1.0f);
    ImGui::SameLine();
    HelpMarker(TR("performance.typical.help"));

    ImGui::Separator();
    ImGui::Text(TR("performance.gpu_settings"));

    // GPU Layers
    ImGui::InputInt(TR("gpu.gpu_layers"), &chat_settings.n_gpu_layers);
    ImGui::SameLine();
    HelpMarker("GPU Layers:\nNumber of model layers to load on GPU.\n0 = CPU only, -1 = all layers on GPU.");

    // Tensor Split
    char tensor_split_buffer[256];
    strcpy(tensor_split_buffer, chat_settings.tensor_split.c_str());
    if (ImGui::InputText("Tensor Split", tensor_split_buffer, sizeof(tensor_split_buffer))) {
        chat_settings.tensor_split = tensor_split_buffer;
    }
    ImGui::SameLine();
    HelpMarker("Tensor Split:\nDistribution of tensors across multiple GPUs.\nExample: '1,1' for equal distribution between 2 GPUs.");

    ImGui::Separator();
    ImGui::Text(TR("performance.memory_management"));

    // Memory Lock - перемещено в GPU settings
    auto& gpu_settings = settings_.gpu();
    ImGui::Checkbox(TR("performance.memory_lock"), &gpu_settings.mlock);
    ImGui::SameLine();
    HelpMarker("Memory Lock:\nLocks model memory in RAM, preventing swapping.\nImproves performance but uses more memory.");

    // No Memory Mapping
    ImGui::Checkbox(TR("performance.disable_mmap"), &gpu_settings.no_mmap);
    ImGui::SameLine();
    HelpMarker("Disable Memory Mapping:\nDisables memory mapping for model loading.\nUse if you have mmap issues.");

    // NUMA Strategy
    const char* numa_options[] = { "none", "distribute", "isolate", "numactl" };
    int numa_idx = 0;
    if (chat_settings.numa == "distribute") numa_idx = 1;
    else if (chat_settings.numa == "isolate") numa_idx = 2;
    else if (chat_settings.numa == "numactl") numa_idx = 3;
    if (ImGui::Combo(TR("performance.numa_strategy"), &numa_idx, numa_options, IM_ARRAYSIZE(numa_options))) {
        switch (numa_idx) {
            case 0: chat_settings.numa = "none"; break;
            case 1: chat_settings.numa = "distribute"; break;
            case 2: chat_settings.numa = "isolate"; break;
            case 3: chat_settings.numa = "numactl"; break;
        }
    }
    ImGui::SameLine();
    HelpMarker("NUMA Strategy:\nMemory distribution strategy in NUMA systems.\n'distribute' = even distribution, 'isolate' = isolate on one node.");

    ImGui::Separator();
    ImGui::Text(TR("performance.lora_finetuning"));

    // LoRA Base Model
    char lora_base_buffer[256];
    strcpy(lora_base_buffer, chat_settings.lora_base.c_str());
    if (ImGui::InputText(TR("performance.lora_base"), lora_base_buffer, sizeof(lora_base_buffer))) {
        chat_settings.lora_base = lora_base_buffer;
    }
    ImGui::SameLine();
    HelpMarker("LoRA Base Model:\nBase model for applying LoRA adapters.");

    ImGui::Separator();
    ImGui::Text(TR("performance.multimodal_grammar"));

    // Multimodal Projector
    char mmproj_buffer[256];
    strcpy(mmproj_buffer, chat_settings.mmproj.c_str());
    if (ImGui::InputText(TR("performance.mm_projector"), mmproj_buffer, sizeof(mmproj_buffer))) {
        chat_settings.mmproj = mmproj_buffer;
    }
    ImGui::SameLine();
    HelpMarker("Multimodal Projector:\nPath to projector file for processing images and other modalities.");

    // Grammar File
    char grammar_buffer[256];
    strcpy(grammar_buffer, chat_settings.grammar.c_str());
    if (ImGui::InputText(TR("performance.grammar_file"), grammar_buffer, sizeof(grammar_buffer))) {
        chat_settings.grammar = grammar_buffer;
    }
    ImGui::SameLine();
    HelpMarker("Grammar File:\nGrammar file for constraining model output (e.g., JSON schema).");

    // Chat Template
    char chat_template_buffer[256];
    strcpy(chat_template_buffer, chat_settings.chat_template.c_str());
    if (ImGui::InputText(TR("performance.chat_template"), chat_template_buffer, sizeof(chat_template_buffer))) {
        chat_settings.chat_template = chat_template_buffer;
    }
    ImGui::SameLine();
    HelpMarker("Chat Template:\nTemplate for formatting chat messages.");

    ImGui::Separator();
    ImGui::Text(TR("performance.logging_debug"));

    // Embedding Mode
    ImGui::Checkbox(TR("performance.embedding_mode"), &chat_settings.embedding);
    ImGui::SameLine();
    HelpMarker("Embedding Mode:\nEnables embedding generation mode instead of text.");

    // Log Format
    const char* log_formats[] = { "text", "json" };
    int log_format_idx = (chat_settings.log_format == "json") ? 1 : 0;
    if (ImGui::Combo(TR("performance.log_format"), &log_format_idx, log_formats, IM_ARRAYSIZE(log_formats))) {
        chat_settings.log_format = log_formats[log_format_idx];
    }
    ImGui::SameLine();
    HelpMarker("Log Format:\nLog output format - text or JSON.");

    // Verbosity Level
    ImGui::InputInt(TR("logging.verbosity.level"), &chat_settings.verbosity);
    ImGui::SameLine();
    HelpMarker("Verbosity Level:\nOutput detail level.\nHigher values show more debug information.");
}

} // namespace ui
} // namespace llama_gui
