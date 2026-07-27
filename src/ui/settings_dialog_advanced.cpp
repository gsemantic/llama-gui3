#include "../include/ui/settings_dialog_advanced.h"
#include "../external/imgui/imgui.h"
#include <iostream>

namespace llama_gui {
namespace ui {

AdvancedDialog::AdvancedDialog(Settings& settings)
    : settings_(settings) {
    new_cv_path_[0] = '\0';
    new_cv_scale_ = 1.0f;
    new_to_pattern_[0] = '\0';
    new_to_buffer_type_[0] = '\0';
}

AdvancedDialog::~AdvancedDialog() = default;

void AdvancedDialog::HelpMarker(const std::string& desc) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void AdvancedDialog::render_control_vector_editor(size_t index) {
    auto& cv_settings = settings_.control_vector();
    auto& cv = cv_settings.control_vectors[index];

    ImGui::PushID(static_cast<int>(index));

    char path_buf[512];
    strncpy(path_buf, cv.path.c_str(), sizeof(path_buf) - 1);
    path_buf[sizeof(path_buf) - 1] = '\0';

    ImGui::SetNextItemWidth(300);
    if (ImGui::InputText("##cv_path", path_buf, sizeof(path_buf))) {
        cv.path = path_buf;
        modified_ = true;
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    float scale = cv.scale;
    if (ImGui::SliderFloat("##cv_scale", &scale, 0.0f, 5.0f, "%.2f")) {
        cv.scale = scale;
        modified_ = true;
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("Remove##remove_cv")) {
        cv_settings.remove_control_vector(index);
        modified_ = true;
        ImGui::PopID();
        return;
    }

    ImGui::PopID();
}

void AdvancedDialog::render_tensor_override_editor(size_t index) {
    auto& to_settings = settings_.tensor_override();
    auto& to = to_settings.overrides[index];

    ImGui::PushID(static_cast<int>(index));

    char pattern_buf[128];
    strncpy(pattern_buf, to.pattern.c_str(), sizeof(pattern_buf) - 1);
    pattern_buf[sizeof(pattern_buf) - 1] = '\0';

    ImGui::SetNextItemWidth(200);
    if (ImGui::InputText("##to_pattern", pattern_buf, sizeof(pattern_buf))) {
        to.pattern = pattern_buf;
        modified_ = true;
    }

    ImGui::SameLine();
    char buf_type_buf[64];
    strncpy(buf_type_buf, to.buffer_type.c_str(), sizeof(buf_type_buf) - 1);
    buf_type_buf[sizeof(buf_type_buf) - 1] = '\0';

    ImGui::SetNextItemWidth(120);
    if (ImGui::InputText("##to_buffer", buf_type_buf, sizeof(buf_type_buf))) {
        to.buffer_type = buf_type_buf;
        modified_ = true;
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("Remove##remove_to")) {
        to_settings.remove_override(index);
        modified_ = true;
        ImGui::PopID();
        return;
    }

    ImGui::PopID();
}

void AdvancedDialog::render_control_vectors_section() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Control Vectors");
    ImGui::Separator();

    auto& cv_settings = settings_.control_vector();

    ImGui::Text("Control vectors allow steering model behavior by adding");
    ImGui::Text("vector offsets to activations at specific layers.");
    ImGui::Separator();

    // Control vectors list
    ImGui::Text("Active Control Vectors:");

    auto& vectors = cv_settings.control_vectors;
    for (size_t i = 0; i < vectors.size(); ++i) {
        render_control_vector_editor(i);
    }

    // Add new control vector
    ImGui::Separator();
    ImGui::Text("Add Control Vector:");

    ImGui::SetNextItemWidth(300);
    ImGui::InputText("Path##new_cv_path", new_cv_path_, sizeof(new_cv_path_));

    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::SliderFloat("Scale##new_cv_scale", &new_cv_scale_, 0.0f, 5.0f, "%.2f");

    ImGui::SameLine();
    if (ImGui::Button("Add##add_cv")) {
        if (new_cv_path_[0] != '\0') {
            cv_settings.add_control_vector(new_cv_path_, new_cv_scale_);
            modified_ = true;
            new_cv_path_[0] = '\0';
            new_cv_scale_ = 1.0f;
        }
    }

    ImGui::Separator();

    // Layer range
    ImGui::Text("Layer Range:");

    int layer_start = cv_settings.control_vector_layer_start;
    int layer_end = cv_settings.control_vector_layer_end;

    ImGui::SetNextItemWidth(100);
    if (ImGui::SliderInt("Start##cv_layer_start", &layer_start, 0, 256)) {
        cv_settings.control_vector_layer_start = layer_start;
        modified_ = true;
    }

    ImGui::SameLine();
    ImGui::Text("to");
    ImGui::SameLine();

    ImGui::SetNextItemWidth(100);
    if (ImGui::SliderInt("End##cv_layer_end", &layer_end, -1, 256)) {
        cv_settings.control_vector_layer_end = layer_end;
        modified_ = true;
    }

    ImGui::SameLine();
    HelpMarker("-1 = all layers to the end");
}

void AdvancedDialog::render_tensor_override_section() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Tensor Override");
    ImGui::Separator();

    auto& to_settings = settings_.tensor_override();

    ImGui::Text("Override buffer types for specific tensors by pattern matching.");
    ImGui::Separator();

    // Tensor overrides list
    ImGui::Text("Active Overrides:");

    auto& overrides = to_settings.overrides;
    for (size_t i = 0; i < overrides.size(); ++i) {
        render_tensor_override_editor(i);
    }

    // Add new override
    ImGui::Separator();
    ImGui::Text("Add Tensor Override:");

    ImGui::SetNextItemWidth(200);
    ImGui::InputText("Pattern##new_to_pattern", new_to_pattern_, sizeof(new_to_pattern_));

    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::InputText("Buffer Type##new_to_buffer", new_to_buffer_type_, sizeof(new_to_buffer_type_));

    ImGui::SameLine();
    if (ImGui::Button("Add##add_to")) {
        if (new_to_pattern_[0] != '\0' && new_to_buffer_type_[0] != '\0') {
            to_settings.add_override(new_to_pattern_, new_to_buffer_type_);
            modified_ = true;
            new_to_pattern_[0] = '\0';
            new_to_buffer_type_[0] = '\0';
        }
    }

    ImGui::SameLine();
    HelpMarker("Buffer types: VRAM, RAM, CUDA, SYCL, etc.");

    // Common patterns
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Common Patterns")) {
        ImGui::Indent();

        ImGui::BulletText("blk.*.ffn_down.* - FFN down projection");
        ImGui::BulletText("blk.*.attn_output.* - Attention output");
        ImGui::BulletText("output.* - Output layer tensors");

        if (ImGui::Button("Add VRAM Override for Output")) {
            to_settings.add_override("output.*", "VRAM");
            modified_ = true;
        }

        ImGui::Unindent();
    }
}

void AdvancedDialog::render_speculative_decoding_section() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Speculative Decoding");
    ImGui::Separator();

    auto& model = settings_.model_loading();

    ImGui::Text("Speculative decoding uses a smaller draft model to generate");
    ImGui::Text("candidate tokens that are then verified by the main model.");
    ImGui::Separator();

    // Draft model path
    {
        char buf[512];
        strncpy(buf, model.model_draft.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        if (ImGui::InputText("Draft Model Path##draft_path", buf, sizeof(buf))) {
            model.model_draft = buf;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Path to draft model for speculative decoding");
    }

    // Draft HF repo
    {
        char buf[256];
        strncpy(buf, model.hf_repo_draft.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        if (ImGui::InputText("Draft HF Repo##draft_hf", buf, sizeof(buf))) {
            model.hf_repo_draft = buf;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Hugging Face repo for draft model");
    }

    ImGui::Separator();
    ImGui::Text("Draft Parameters:");

    // Draft max
    {
        int draft_max = model.draft_max;
        if (ImGui::SliderInt("Draft Max Tokens##draft_max", &draft_max, 0, 64)) {
            model.draft_max = draft_max;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Maximum tokens to draft per step");
    }

    // Draft min
    {
        int draft_min = model.draft_min;
        if (ImGui::SliderInt("Draft Min Tokens##draft_min", &draft_min, 0, 16)) {
            model.draft_min = draft_min;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Minimum tokens to draft per step");
    }

    // Draft p-min
    {
        float draft_p_min = model.draft_p_min;
        if (ImGui::SliderFloat("Draft P-Min##draft_pmin", &draft_p_min, 0.0f, 1.0f, "%.2f")) {
            model.draft_p_min = draft_p_min;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Minimum probability for draft tokens");
    }

    // Draft GPU layers
    {
        auto& gpu = settings_.gpu();
        int draft_gpu = gpu.n_gpu_layers_draft;
        if (ImGui::SliderInt("Draft GPU Layers##draft_gpu", &draft_gpu, -1, 256)) {
            gpu.n_gpu_layers_draft = draft_gpu;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("-1 = auto, same as main model");
    }

    // Draft cache types
    if (ImGui::CollapsingHeader("Draft Cache Types")) {
        ImGui::Indent();

        auto& cache = settings_.cache();

        int k_draft = static_cast<int>(cache.cache_type_k_draft);
        int v_draft = static_cast<int>(cache.cache_type_v_draft);

        ImGui::Text("Draft Cache K:");
        ImGui::SameLine();
        if (ImGui::Combo("##draft_cache_k", &k_draft, "F32\0F16\0BF16\0Q8_0\0Q4_0\0Q4_1\0IQ4_NL\0Q5_0\0Q5_1\0")) {
            cache.cache_type_k_draft = static_cast<llama_gui::core::CacheSettings::CacheType>(k_draft);
            modified_ = true;
        }

        ImGui::Text("Draft Cache V:");
        ImGui::SameLine();
        if (ImGui::Combo("##draft_cache_v", &v_draft, "F32\0F16\0BF16\0Q8_0\0Q4_0\0Q4_1\0IQ4_NL\0Q5_0\0Q5_1\0")) {
            cache.cache_type_v_draft = static_cast<llama_gui::core::CacheSettings::CacheType>(v_draft);
            modified_ = true;
        }

        ImGui::Unindent();
    }
}

void AdvancedDialog::render() {
    // =========================================================================
    // Control Vectors Section
    // =========================================================================
    render_control_vectors_section();

    ImGui::Separator();

    // =========================================================================
    // Tensor Override Section
    // =========================================================================
    render_tensor_override_section();

    ImGui::Separator();

    // =========================================================================
    // Speculative Decoding Section
    // =========================================================================
    render_speculative_decoding_section();

    // =========================================================================
    // Summary
    // =========================================================================
    ImGui::Separator();
    ImGui::Text("Configuration Summary:");

    auto& cv = settings_.control_vector();
    auto& to = settings_.tensor_override();
    auto& model = settings_.model_loading();

    ImGui::BulletText("Control Vectors: %zu (layers %d-%d)",
                      cv.control_vectors.size(),
                      cv.control_vector_layer_start,
                      cv.control_vector_layer_end);

    ImGui::BulletText("Tensor Overrides: %zu", to.overrides.size());

    if (!model.model_draft.empty()) {
        ImGui::BulletText("Draft Model: %s", model.model_draft.c_str());
        ImGui::BulletText("  Max=%d, Min=%d, P-Min=%.2f",
                          model.draft_max, model.draft_min, model.draft_p_min);
    } else {
        ImGui::BulletText("Draft Model: Not configured");
    }
}

} // namespace ui
} // namespace llama_gui
