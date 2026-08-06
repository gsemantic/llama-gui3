#include "../include/ui/settings_dialog_sampling_basic.h"
#include "../external/imgui/imgui.h"
#include <iostream>
#include <sstream>

namespace llama_gui {
namespace ui {

SamplingBasicDialog::SamplingBasicDialog(Settings& settings)
    : settings_(settings) {}

SamplingBasicDialog::~SamplingBasicDialog() = default;

void SamplingBasicDialog::HelpMarker(const std::string& desc) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void SamplingBasicDialog::render() {
    auto& sampling = settings_.sampling();

    // =========================================================================
    // Basic Sampling Section
    // =========================================================================
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Basic Sampling");
    ImGui::Separator();

    // Temperature
    {
        float temp = sampling.temperature;
        if (ImGui::SliderFloat("Temperature##temp", &temp, 0.0f, 2.0f, "%.2f")) {
            sampling.temperature = temp;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Controls randomness. Higher = more creative, lower = more deterministic");
    }

    // Top-K
    {
        int top_k = sampling.top_k;
        if (ImGui::SliderInt("Top-K##top_k", &top_k, 0, 200)) {
            sampling.top_k = top_k;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Limits sampling to top K most likely tokens. 0 = disabled");
    }

    // Top-P (Nucleus)
    {
        float top_p = sampling.top_p;
        if (ImGui::SliderFloat("Top-P (Nucleus)##top_p", &top_p, 0.0f, 1.0f, "%.3f")) {
            sampling.top_p = top_p;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Samples from smallest set of tokens with cumulative probability P");
    }

    // Min-P
    {
        float min_p = sampling.min_p;
        if (ImGui::SliderFloat("Min-P##min_p", &min_p, 0.0f, 1.0f, "%.3f")) {
            sampling.min_p = min_p;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Minimum probability threshold for token sampling");
    }

    ImGui::Separator();

    // =========================================================================
    // Advanced Sampling Section
    // =========================================================================
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Advanced Sampling");
    ImGui::Separator();

    // Typical Sampling
    {
        float typical_p = sampling.typical_p;
        if (ImGui::SliderFloat("Typical-P##typical_p", &typical_p, 0.0f, 1.0f, "%.3f")) {
            sampling.typical_p = typical_p;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Locally typical sampling. 1.0 = disabled");
    }

    // Tail-Free Sampling (TFS)
    {
        float tfs_z = sampling.tfs_z;
        if (ImGui::SliderFloat("Tail-Free (TFS-Z)##tfs_z", &tfs_z, 0.0f, 1.0f, "%.3f")) {
            sampling.tfs_z = tfs_z;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Tail-free sampling. Removes long tail of low-probability tokens. 1.0 = disabled");
    }

    ImGui::Separator();

    // =========================================================================
    // Mirostat Section
    // =========================================================================
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Mirostat (Adaptive Entropy Control)");
    ImGui::Separator();

    // Mirostat Mode
    ImGui::Text("Mirostat Mode:");
    ImGui::SameLine();

    const char* mirostat_modes[] = { "Disabled", "Mirostat v1", "Mirostat v2" };
    int current_mirostat_mode = sampling.mirostat_mode;

    if (ImGui::Combo("##mirostat_mode", &current_mirostat_mode, mirostat_modes, 3)) {
        sampling.mirostat_mode = current_mirostat_mode;
        modified_ = true;
    }
    ImGui::SameLine();
    HelpMarker("Mirostat adjusts temperature dynamically to maintain target entropy");

    // Mirostat parameters (only enabled if mode > 0)
    if (sampling.mirostat_mode > 0) {
        ImGui::Indent();

        // Mirostat Tau (target entropy)
        {
            float tau = sampling.mirostat_tau;
            if (ImGui::SliderFloat("Target Entropy (Tau)##mirostat_tau", &tau, 0.0f, 10.0f, "%.2f")) {
                sampling.mirostat_tau = tau;
                modified_ = true;
            }
            ImGui::SameLine();
            HelpMarker("Target entropy (surprise) level. Higher = more random");
        }

        // Mirostat Eta (learning rate)
        {
            float eta = sampling.mirostat_eta;
            if (ImGui::SliderFloat("Learning Rate (Eta)##mirostat_eta", &eta, 0.0f, 1.0f, "%.3f")) {
                sampling.mirostat_eta = eta;
                modified_ = true;
            }
            ImGui::SameLine();
            HelpMarker("How quickly Mirostat adjusts temperature. Higher = faster adaptation");
        }

        ImGui::Unindent();
    }

    // =========================================================================
    // Preset Quick Select
    // =========================================================================
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Quick Presets")) {
        ImGui::Indent();

        if (ImGui::Button("Creative Writing")) {
            sampling.temperature = 1.0f;
            sampling.top_p = 0.95f;
            sampling.top_k = 50;
            sampling.mirostat_mode = 0;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("High temperature, diverse sampling for creative tasks");

        if (ImGui::Button("Balanced")) {
            sampling.temperature = 0.8f;
            sampling.top_p = 0.9f;
            sampling.top_k = 40;
            sampling.mirostat_mode = 0;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Default balanced settings");

        if (ImGui::Button("Precise/Code")) {
            sampling.temperature = 0.2f;
            sampling.top_p = 0.5f;
            sampling.top_k = 20;
            sampling.mirostat_mode = 0;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Low temperature for deterministic output");

        if (ImGui::Button("Mirostat Balanced")) {
            sampling.temperature = 0.8f;
            sampling.mirostat_mode = 2;
            sampling.mirostat_tau = 5.0f;
            sampling.mirostat_eta = 0.1f;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Mirostat v2 with default parameters");

        ImGui::Unindent();
    }

    // =========================================================================
    // Current Settings Display
    // =========================================================================
    ImGui::Separator();
    ImGui::Text("Current Configuration:");
    ImGui::BulletText("Temperature: %.3f", sampling.temperature);
    ImGui::BulletText("Top-K: %d", sampling.top_k);
    ImGui::BulletText("Top-P: %.3f", sampling.top_p);
    ImGui::BulletText("Min-P: %.3f", sampling.min_p);
    ImGui::BulletText("Typical-P: %.3f", sampling.typical_p);
    ImGui::BulletText("TFS-Z: %.3f", sampling.tfs_z);

    if (sampling.mirostat_mode > 0) {
        ImGui::BulletText("Mirostat: Mode %d (Tau=%.2f, Eta=%.3f)",
                          sampling.mirostat_mode, sampling.mirostat_tau, sampling.mirostat_eta);
    } else {
        ImGui::BulletText("Mirostat: Disabled");
    }
}

} // namespace ui
} // namespace llama_gui
