#include "../include/ui/settings_dialog_sampling_advanced.h"
#include "../external/imgui/imgui.h"
#include <iostream>
#include <sstream>

namespace llama_gui {
namespace ui {

SamplingAdvancedDialog::SamplingAdvancedDialog(Settings& settings)
    : settings_(settings) {
    new_dry_breaker_[0] = '\0';
}

SamplingAdvancedDialog::~SamplingAdvancedDialog() = default;

void SamplingAdvancedDialog::HelpMarker(const std::string& desc) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void SamplingAdvancedDialog::render() {
    auto& sampling = settings_.sampling();

    // =========================================================================
    // DRY Sampling Section (Don't Repeat Yourself)
    // =========================================================================
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "DRY Sampling (Anti-Repetition)");
    ImGui::Separator();

    // DRY Multiplier
    {
        float mult = sampling.dry_multiplier;
        if (ImGui::SliderFloat("DRY Multiplier##dry_mult", &mult, 0.0f, 10.0f, "%.3f")) {
            sampling.dry_multiplier = mult;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Strength of DRY penalty. 0 = disabled");
    }

    // DRY Base
    {
        float base = sampling.dry_base;
        if (ImGui::SliderFloat("DRY Base##dry_base", &base, 1.0f, 10.0f, "%.2f")) {
            sampling.dry_base = base;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Base value for DRY calculation");
    }

    // DRY Allowed Length
    {
        int len = sampling.dry_allowed_length;
        if (ImGui::SliderInt("DRY Allowed Length##dry_len", &len, 1, 10)) {
            sampling.dry_allowed_length = len;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Number of tokens allowed to repeat before penalty applies");
    }

    // DRY Penalty Last N
    {
        int last_n = sampling.dry_penalty_last_n;
        if (ImGui::SliderInt("DRY Penalty Last N##dry_last_n", &last_n, -1, 512)) {
            sampling.dry_penalty_last_n = last_n;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Number of tokens to check for repetition. -1 = context size");
    }

    // DRY Sequence Breakers
    ImGui::Separator();
    ImGui::Text("DRY Sequence Breakers:");
    ImGui::SameLine();
    HelpMarker("Tokens that reset the repetition counter");

    auto& breakers = sampling.dry_sequence_breakers;
    for (size_t i = 0; i < breakers.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));

        char buf[64];
        strncpy(buf, breakers[i].c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        ImGui::SetNextItemWidth(150);
        if (ImGui::InputText("##dry_breaker", buf, sizeof(buf))) {
            breakers[i] = buf;
            modified_ = true;
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Remove##remove_breaker")) {
            breakers.erase(breakers.begin() + i);
            modified_ = true;
            ImGui::PopID();
            break;
        }

        ImGui::PopID();
    }

    // Add new breaker
    ImGui::SetNextItemWidth(150);
    ImGui::InputText("New Breaker##new_breaker", new_dry_breaker_, sizeof(new_dry_breaker_));

    ImGui::SameLine();
    if (ImGui::Button("Add##add_breaker")) {
        if (new_dry_breaker_[0] != '\0') {
            breakers.push_back(new_dry_breaker_);
            modified_ = true;
            new_dry_breaker_[0] = '\0';
        }
    }

    // Default breakers preset
    ImGui::SameLine();
    if (ImGui::Button("Reset to Defaults")) {
        breakers = {"\n", ":", "\"", "*"};
        modified_ = true;
    }

    // =========================================================================
    // XTC Sampling Section (Exclusion-based)
    // =========================================================================
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "XTC Sampling (Exclusion-based)");
    ImGui::Separator();

    // XTC Probability
    {
        float prob = sampling.xtc_probability;
        if (ImGui::SliderFloat("XTC Probability##xtc_prob", &prob, 0.0f, 1.0f, "%.2f")) {
            sampling.xtc_probability = prob;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Probability of applying XTC sampling");
    }

    // XTC Threshold
    {
        float thresh = sampling.xtc_threshold;
        if (ImGui::SliderFloat("XTC Threshold##xtc_thresh", &thresh, 0.0f, 1.0f, "%.3f")) {
            sampling.xtc_threshold = thresh;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Minimum probability for token exclusion");
    }

    // =========================================================================
    // Dynamic Temperature Section
    // =========================================================================
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Dynamic Temperature");
    ImGui::Separator();

    // Dynatemp Range
    {
        float range = sampling.dynatemp_range;
        if (ImGui::SliderFloat("Dynatemp Range##dyna_range", &range, 0.0f, 2.0f, "%.3f")) {
            sampling.dynatemp_range = range;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Range of temperature variation. 0 = disabled");
    }

    // Dynatemp Exponent
    {
        float exp = sampling.dynatemp_exp;
        if (ImGui::SliderFloat("Dynatemp Exponent##dyna_exp", &exp, 0.1f, 5.0f, "%.2f")) {
            sampling.dynatemp_exp = exp;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Exponent for temperature curve shaping");
    }

    // =========================================================================
    // Penalties Section
    // =========================================================================
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Penalties");
    ImGui::Separator();

    // Repeat Penalty
    {
        float penalty = sampling.repeat_penalty;
        if (ImGui::SliderFloat("Repeat Penalty##repeat_pen", &penalty, 0.0f, 5.0f, "%.3f")) {
            sampling.repeat_penalty = penalty;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Penalty for repeating tokens. 1.0 = no penalty");
    }

    // Presence Penalty
    {
        float penalty = sampling.presence_penalty;
        if (ImGui::SliderFloat("Presence Penalty##presence_pen", &penalty, -2.0f, 2.0f, "%.3f")) {
            sampling.presence_penalty = penalty;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Penalize tokens based on presence in context");
    }

    // Frequency Penalty
    {
        float penalty = sampling.frequency_penalty;
        if (ImGui::SliderFloat("Frequency Penalty##freq_pen", &penalty, -2.0f, 2.0f, "%.3f")) {
            sampling.frequency_penalty = penalty;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Penalize tokens based on frequency in context");
    }

    ImGui::Separator();

    // Repeat Last N
    {
        int last_n = sampling.repeat_last_n;
        if (ImGui::SliderInt("Repeat Last N##repeat_last_n", &last_n, 0, 512)) {
            sampling.repeat_last_n = last_n;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Number of tokens to check for repetition penalty");
    }

    // Penalize Newline
    {
        bool penalize_nl = sampling.penalize_nl;
        if (ImGui::Checkbox("Penalize Newline##penalize_nl", &penalize_nl)) {
            sampling.penalize_nl = penalize_nl;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Apply penalties to newline tokens");
    }

    // Ignore EOS
    {
        bool ignore_eos = sampling.ignore_eos;
        if (ImGui::Checkbox("Ignore EOS##ignore_eos", &ignore_eos)) {
            sampling.ignore_eos = ignore_eos;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Continue generation even after EOS token");
    }

    // =========================================================================
    // Sampler Order Section
    // =========================================================================
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Sampler Order");
    ImGui::Separator();

    // Enable custom sampler order
    {
        bool use_custom = sampling.use_custom_sampler_order;
        if (ImGui::Checkbox("Use Custom Sampler Order##use_custom_order", &use_custom)) {
            sampling.use_custom_sampler_order = use_custom;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Enable custom ordering of samplers");
    }

    if (sampling.use_custom_sampler_order) {
        ImGui::Indent();

        char buf[64];
        strncpy(buf, sampling.samplers_order.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        if (ImGui::InputText("Sampler Sequence##sampler_seq", buf, sizeof(buf))) {
            sampling.samplers_order = buf;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Order: e=entropy, d=dry, s=softmax, k=top-k, p=top-p, y=typical, m=min-p, x=xtc");

        // Preset buttons
        if (ImGui::Button("Default")) {
            sampling.samplers_order = "edskypmxt";
            modified_ = true;
        }
        ImGui::SameLine();

        if (ImGui::Button("Top-K First")) {
            sampling.samplers_order = "edkypmxts";
            modified_ = true;
        }
        ImGui::SameLine();

        if (ImGui::Button("Top-P First")) {
            sampling.samplers_order = "edpkymxts";
            modified_ = true;
        }

        ImGui::Unindent();
    }

    // =========================================================================
    // Current Configuration Summary
    // =========================================================================
    ImGui::Separator();
    ImGui::Text("Active Configuration:");

    ImGui::BulletText("DRY: multiplier=%.3f, base=%.2f, allowed=%d",
                      sampling.dry_multiplier, sampling.dry_base, sampling.dry_allowed_length);

    if (sampling.dry_multiplier > 0) {
        ImGui::BulletText("  Breakers: ");
        ImGui::SameLine();
        std::string breakers_str;
        for (size_t i = 0; i < sampling.dry_sequence_breakers.size(); ++i) {
            if (i > 0) breakers_str += ", ";
            breakers_str += "\"" + sampling.dry_sequence_breakers[i] + "\"";
        }
        ImGui::Text("%s", breakers_str.c_str());
    }

    ImGui::BulletText("XTC: probability=%.2f, threshold=%.3f",
                      sampling.xtc_probability, sampling.xtc_threshold);

    ImGui::BulletText("Dynatemp: range=%.3f, exp=%.2f",
                      sampling.dynatemp_range, sampling.dynatemp_exp);

    ImGui::BulletText("Penalties: repeat=%.3f, presence=%.3f, frequency=%.3f, last_n=%d",
                      sampling.repeat_penalty, sampling.presence_penalty,
                      sampling.frequency_penalty, sampling.repeat_last_n);

    if (sampling.use_custom_sampler_order) {
        ImGui::BulletText("Sampler Order: %s", sampling.samplers_order.c_str());
    }
}

} // namespace ui
} // namespace llama_gui
