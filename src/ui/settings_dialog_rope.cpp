#include "../include/ui/settings_dialog_rope.h"
#include "../include/ui/localization_manager.h"
#include "../external/imgui/imgui.h"
#include <iostream>
#include <sstream>

namespace llama_gui {
namespace ui {

RoPEDialog::RoPEDialog(Settings& settings)
    : settings_(settings) {}

RoPEDialog::~RoPEDialog() = default;

void RoPEDialog::HelpMarker(const std::string& desc) {
    ImGui::TextDisabled(TR("help_icon"));
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void RoPEDialog::render() {
    auto& rope = settings_.rope();

    // =========================================================================
    // RoPE Scaling Section
    // =========================================================================
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), TR("rope.scaling"));
    ImGui::Separator();

    ImGui::Text(TR("rope.scaling_mode"));
    ImGui::SameLine();

    const char* scaling_modes[] = {
        TR("rope.scaling.none"),
        TR("rope.scaling.linear"),
        TR("rope.scaling.yarn")
    };

    int current_mode = 0;
    if (rope.rope_scaling == llama_gui::core::RoPESettings::RopeScaling::Linear) {
        current_mode = 1;
    } else if (rope.rope_scaling == llama_gui::core::RoPESettings::RopeScaling::Yarn) {
        current_mode = 2;
    }

    if (ImGui::Combo(TR("rope.scaling_mode"), &current_mode, scaling_modes, 3)) {
        switch (current_mode) {
            case 0: rope.set_scaling("none"); break;
            case 1: rope.set_scaling("linear"); break;
            case 2: rope.set_scaling("yarn"); break;
        }
        modified_ = true;
    }
    ImGui::SameLine();
    HelpMarker(TR("rope.scaling.help"));

    ImGui::Separator();

    // =========================================================================
    // RoPE Parameters Section
    // =========================================================================
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), TR("rope.parameters"));
    ImGui::Separator();

    // RoPE Scale
    {
        float scale = rope.rope_scale;
        if (ImGui::SliderFloat(TR("rope.scale"), &scale, 0.1f, 10.0f, "%.2f")) {
            rope.rope_scale = scale;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("rope.scale.help"));
    }

    // RoPE Frequency Base
    {
        float base = rope.rope_freq_base;
        if (ImGui::SliderFloat(TR("rope.freq_base"), &base, 0.0f, 1000000.0f, "%.0f")) {
            rope.rope_freq_base = base;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("rope.freq_base.help"));
    }

    // RoPE Frequency Scale
    {
        float scale = rope.rope_freq_scale;
        if (ImGui::SliderFloat(TR("rope.freq_scale"), &scale, 0.1f, 10.0f, "%.3f")) {
            rope.rope_freq_scale = scale;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("rope.freq_scale.help"));
    }

    ImGui::Separator();

    // =========================================================================
    // YaRN Section (Yet another RoPE)
    // =========================================================================
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), TR("rope.yarn_params"));
    ImGui::Separator();

    // Show YaRN section always, but highlight when YaRN is selected
    if (rope.rope_scaling == llama_gui::core::RoPESettings::RopeScaling::Yarn) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.0f, 1.0f));
        ImGui::Text(TR("rope.yarn_active"));
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    ImGui::Text(TR("rope.yarn_desc"));

    // YaRN Original Context
    {
        int orig_ctx = rope.yarn_orig_ctx;
        if (ImGui::SliderInt(TR("rope.yarn.orig_ctx"), &orig_ctx, 0, 131072)) {
            rope.yarn_orig_ctx = orig_ctx;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("rope.yarn.orig_ctx.help"));
    }

    // YaRN Extension Factor
    {
        float ext_factor = rope.yarn_ext_factor;
        if (ImGui::SliderFloat(TR("rope.yarn.ext_factor"), &ext_factor, -1.0f, 10.0f, "%.2f")) {
            rope.yarn_ext_factor = ext_factor;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("rope.yarn.ext_factor.help"));
    }

    // YaRN Attention Factor
    {
        float attn_factor = rope.yarn_attn_factor;
        if (ImGui::SliderFloat(TR("rope.yarn.attn_factor"), &attn_factor, 0.1f, 5.0f, "%.3f")) {
            rope.yarn_attn_factor = attn_factor;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("rope.yarn.attn_factor.help"));
    }

    ImGui::Separator();

    // YaRN Beta Slow
    {
        float beta_slow = rope.yarn_beta_slow;
        if (ImGui::SliderFloat(TR("rope.yarn.beta_slow"), &beta_slow, 0.0f, 100.0f, "%.2f")) {
            rope.yarn_beta_slow = beta_slow;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("rope.yarn.beta_slow.help"));
    }

    // YaRN Beta Fast
    {
        float beta_fast = rope.yarn_beta_fast;
        if (ImGui::SliderFloat(TR("rope.yarn.beta_fast"), &beta_fast, 0.0f, 100.0f, "%.2f")) {
            rope.yarn_beta_fast = beta_fast;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("rope.yarn.beta_fast.help"));
    }

    // =========================================================================
    // Presets
    // =========================================================================
    ImGui::Separator();
    if (ImGui::CollapsingHeader(TR("rope.presets"))) {
        ImGui::Indent();

        if (ImGui::Button(TR("rope.preset.llama2_4k"))) {
            rope.set_scaling("none");
            rope.rope_scale = 1.0f;
            rope.rope_freq_base = 10000.0f;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("rope.preset.llama2_4k.help"));

        if (ImGui::Button(TR("rope.preset.llama3_8k"))) {
            rope.set_scaling("linear");
            rope.rope_scale = 2.0f;
            rope.rope_freq_base = 500000.0f;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("rope.preset.llama3_8k.help"));

        if (ImGui::Button(TR("rope.preset.yarn_32k"))) {
            rope.set_scaling("yarn");
            rope.yarn_orig_ctx = 4096;
            rope.yarn_ext_factor = 4.0f;
            rope.yarn_attn_factor = 1.0f;
            rope.yarn_beta_slow = 1.0f;
            rope.yarn_beta_fast = 32.0f;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("rope.preset.yarn_32k.help"));

        if (ImGui::Button(TR("rope.preset.yarn_128k"))) {
            rope.set_scaling("yarn");
            rope.yarn_orig_ctx = 4096;
            rope.yarn_ext_factor = 16.0f;
            rope.yarn_attn_factor = 1.0f;
            rope.yarn_beta_slow = 1.0f;
            rope.yarn_beta_fast = 32.0f;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("rope.preset.yarn_128k.help"));

        ImGui::Unindent();
    }

    // =========================================================================
    // Current Configuration Summary
    // =========================================================================
    ImGui::Separator();
    ImGui::Text(TR("rope.current_config"));

    std::string scaling_str;
    switch (rope.rope_scaling) {
        case llama_gui::core::RoPESettings::RopeScaling::None:   scaling_str = TR("rope.scaling.none"); break;
        case llama_gui::core::RoPESettings::RopeScaling::Linear: scaling_str = TR("rope.scaling.linear"); break;
        case llama_gui::core::RoPESettings::RopeScaling::Yarn:   scaling_str = TR("rope.scaling.yarn"); break;
    }

    ImGui::BulletText(TR("rope.config.scaling_mode"), scaling_str.c_str());
    ImGui::BulletText(TR("rope.config.scale"), rope.rope_scale);
    ImGui::BulletText(TR("rope.config.freq_base"), rope.rope_freq_base);
    ImGui::BulletText(TR("rope.config.freq_scale"), rope.rope_freq_scale);

    if (rope.rope_scaling == llama_gui::core::RoPESettings::RopeScaling::Yarn) {
        ImGui::BulletText(TR("rope.config.yarn"),
                          rope.yarn_orig_ctx, rope.yarn_ext_factor, rope.yarn_attn_factor,
                          rope.yarn_beta_slow, rope.yarn_beta_fast);
    }
}

} // namespace ui
} // namespace llama_gui
