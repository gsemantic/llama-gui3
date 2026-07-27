#include "../include/ui/settings_dialog_gpu.h"
#include "../external/imgui/imgui.h"
#include <iostream>
#include <sstream>

namespace llama_gui {
namespace ui {

GPUSettingsDialog::GPUSettingsDialog(Settings& settings)
    : settings_(settings) {
    // Инициализация буфера tensor split
    const auto& gpu = settings_.gpu();
    strncpy(tensor_split_buf_, gpu.tensor_split.c_str(), sizeof(tensor_split_buf_) - 1);
    tensor_split_buf_[sizeof(tensor_split_buf_) - 1] = '\0';
}

GPUSettingsDialog::~GPUSettingsDialog() = default;

void GPUSettingsDialog::HelpMarker(const std::string& desc) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void GPUSettingsDialog::render() {
    auto& gpu = settings_.gpu();

    // =========================================================================
    // GPU Offload Section
    // =========================================================================
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "GPU Offload");
    ImGui::Separator();

    // GPU layers slider
    {
        int n_gpu_layers = gpu.n_gpu_layers;
        if (ImGui::SliderInt("GPU Layers##n_gpu_layers", &n_gpu_layers, 0, 256)) {
            gpu.n_gpu_layers = n_gpu_layers;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Number of layers to offload to GPU (-ngl, --n-gpu-layers)");
    }

    // GPU layers draft
    {
        int n_gpu_layers_draft = gpu.n_gpu_layers_draft;
        if (ImGui::SliderInt("Draft GPU Layers##n_gpu_layers_draft", &n_gpu_layers_draft, -1, 256)) {
            gpu.n_gpu_layers_draft = n_gpu_layers_draft;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("GPU layers for draft model (-1 = auto)");
    }

    ImGui::Separator();

    // Split mode selector
    ImGui::Text("Split Mode:");
    ImGui::SameLine();

    const char* split_modes[] = { "None", "Layer", "Row" };
    int current_split_mode = 0;

    switch (gpu.split_mode) {
        case llama_gui::core::GPUSettings::SplitMode::None:   current_split_mode = 0; break;
        case llama_gui::core::GPUSettings::SplitMode::Layer:  current_split_mode = 1; break;
        case llama_gui::core::GPUSettings::SplitMode::Row:    current_split_mode = 2; break;
    }

    if (ImGui::Combo("##split_mode", &current_split_mode, split_modes, 3)) {
        switch (current_split_mode) {
            case 0: gpu.set_split_mode("none"); break;
            case 1: gpu.set_split_mode("layer"); break;
            case 2: gpu.set_split_mode("row"); break;
        }
        modified_ = true;
    }
    ImGui::SameLine();
    HelpMarker("How to split tensors across GPUs (-sm, --split-mode)");

    ImGui::Separator();

    // Main GPU
    {
        int main_gpu = gpu.main_gpu;
        if (ImGui::SliderInt("Main GPU##main_gpu", &main_gpu, 0, 16)) {
            gpu.main_gpu = main_gpu;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Main GPU index (-mg, --main-gpu)");
    }

    // Tensor split
    {
        if (ImGui::InputText("Tensor Split##tensor_split", tensor_split_buf_, sizeof(tensor_split_buf_))) {
            gpu.tensor_split = tensor_split_buf_;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Comma-separated ratios (e.g., \"3,2,1\") for each GPU");
    }

    // =========================================================================
    // Memory Section
    // =========================================================================
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Memory Options");
    ImGui::Separator();

    // Mlock (перемещено из chat_settings в gpu_settings)
    {
        bool mlock = gpu.mlock;
        if (ImGui::Checkbox("Lock Memory (mlock)##mlock", &mlock)) {
            gpu.mlock = mlock;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Lock model memory in RAM (--mlock)");
    }

    // No mmap
    {
        bool no_mmap = gpu.no_mmap;
        if (ImGui::Checkbox("Disable mmap##no_mmap", &no_mmap)) {
            gpu.no_mmap = no_mmap;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Disable memory mapping (--no-mmap)");
    }

    ImGui::Separator();

    // No op offload
    {
        bool no_op_offload = gpu.no_op_offload;
        if (ImGui::Checkbox("No Op Offload##no_op_offload", &no_op_offload)) {
            gpu.no_op_offload = no_op_offload;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Do not offload operations to GPU");
    }

    // No KV offload
    {
        bool no_kv_offload = gpu.no_kv_offload;
        if (ImGui::Checkbox("No KV Offload##no_kv_offload", &no_kv_offload)) {
            gpu.no_kv_offload = no_kv_offload;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Do not offload KV cache to GPU (-nkvo)");
    }

    // No warmup
    {
        bool no_warmup = gpu.no_warmup;
        if (ImGui::Checkbox("No Warmup##no_warmup", &no_warmup)) {
            gpu.no_warmup = no_warmup;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Skip GPU memory warmup");
    }

    // =========================================================================
    // Optimization Section
    // =========================================================================
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Optimization");
    ImGui::Separator();

    // Flash Attention
    ImGui::Text("Flash Attention:");
    ImGui::SameLine();

    const char* fa_modes[] = { "Auto", "Enabled", "Disabled" };
    int current_fa = static_cast<int>(gpu.flash_attn);

    if (ImGui::Combo("##flash_attn", &current_fa, fa_modes, 3)) {
        gpu.flash_attn = static_cast<llama_gui::core::GPUSettings::FlashAttention>(current_fa);
        modified_ = true;
    }
    ImGui::SameLine();
    HelpMarker("Flash Attention mode (-fa, --flash-attn)");

    ImGui::Separator();

    // Defrag threshold
    {
        float defrag_thold = gpu.defrag_thold;
        if (ImGui::SliderFloat("Defrag Threshold##defrag_thold", &defrag_thold, 0.0f, 1.0f, "%.2f")) {
            gpu.defrag_thold = defrag_thold;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Memory defragmentation threshold (-dt, --defrag-thold)");
    }

    // =========================================================================
    // Device Info
    // =========================================================================
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Device Information")) {
        ImGui::Indent();

        // TODO: Запрос информации об устройствах
        ImGui::Text("Available GPUs:");
        ImGui::BulletText("No GPU information available");

        ImGui::Separator();
        ImGui::Text("Memory Usage:");
        ImGui::BulletText("VRAM: N/A");
        ImGui::BulletText("RAM: N/A");

        ImGui::Unindent();
    }
}

} // namespace ui
} // namespace llama_gui
