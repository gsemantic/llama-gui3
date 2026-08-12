#include "../include/ui/settings_dialog_model.h"
#include "../include/ui/localization_manager.h"
#include "../external/imgui/imgui.h"
#include <iostream>
#include "../include/ui/input_text_context_menu.h"

namespace llama_gui {
namespace ui {

ModelSettingsDialog::ModelSettingsDialog(Settings& settings)
    : settings_(settings) {
    new_lora_path_[0] = '\0';
}

ModelSettingsDialog::~ModelSettingsDialog() = default;

void ModelSettingsDialog::HelpMarker(const std::string& desc) {
    ImGui::TextDisabled(TR("help_icon"));
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void ModelSettingsDialog::render() {
    auto& model = settings_.model_loading();

    // =========================================================================
    // Model Files Section
    // =========================================================================
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), TR("model.files"));
    ImGui::Separator();

    // Model path
    {
        char buf[512];
        strncpy(buf, model.model_path.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        if (ImGui::InputText(TR("model.path"), buf, sizeof(buf))) {
            settings_.set_model_path(buf);
            modified_ = true;
        }
        InputTextContextMenu();
        ImGui::SameLine();
        HelpMarker(TR("model.path.help"));

        if (ImGui::Button(TR("model.browse"))) {
            if (browse_callback_) {
                browse_callback_([this](const std::string& path) {
                    if (!path.empty()) {
                        settings_.set_model_path(path);
                        modified_ = true;
                    }
                });
            }
        }
    }

    // Model URL
    {
        char buf[512];
        strncpy(buf, model.model_url.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        if (ImGui::InputText(TR("model.url"), buf, sizeof(buf))) {
            model.model_url = buf;
            modified_ = true;
        }
        InputTextContextMenu();
        ImGui::SameLine();
        HelpMarker(TR("model.url.help"));
    }

    // Model alias
    {
        char buf[128];
        strncpy(buf, model.model_alias.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        if (ImGui::InputText(TR("model.alias"), buf, sizeof(buf))) {
            model.model_alias = buf;
            modified_ = true;
        }
        InputTextContextMenu();
        ImGui::SameLine();
        HelpMarker(TR("model.alias.help"));
    }

    // Hugging Face section
    ImGui::Separator();
    if (ImGui::CollapsingHeader(TR("model.huggingface"))) {
        ImGui::Indent();

        // HF Repo
        {
            char buf[256];
            strncpy(buf, model.hf_repo.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            if (ImGui::InputText(TR("model.hf_repo"), buf, sizeof(buf))) {
                model.hf_repo = buf;
                modified_ = true;
            }
            InputTextContextMenu();
            ImGui::SameLine();
            HelpMarker(TR("model.hf_repo.help"));
        }

        // HF File
        {
            char buf[256];
            strncpy(buf, model.hf_file.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            if (ImGui::InputText(TR("model.hf_file"), buf, sizeof(buf))) {
                model.hf_file = buf;
                modified_ = true;
            }
            InputTextContextMenu();
            ImGui::SameLine();
            HelpMarker(TR("model.hf_file.help"));
        }

        // HF Token
        {
            char buf[256];
            strncpy(buf, model.hf_token.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            if (ImGui::InputText(TR("model.hf_token"), buf, sizeof(buf))) {
                model.hf_token = buf;
                modified_ = true;
            }
            InputTextContextMenu();
            ImGui::SameLine();
            HelpMarker(TR("model.hf_token.help"));
        }

        ImGui::Unindent();
    }

    // =========================================================================
    // Draft Model Section
    // =========================================================================
    ImGui::Separator();
    if (ImGui::CollapsingHeader(TR("model.draft_model"))) {
        ImGui::Indent();

        // Draft model path
        {
            char buf[512];
            strncpy(buf, model.model_draft.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            if (ImGui::InputText(TR("model.draft.path"), buf, sizeof(buf))) {
                model.model_draft = buf;
                modified_ = true;
            }
            InputTextContextMenu();
            ImGui::SameLine();
            HelpMarker(TR("model.draft.path.help"));
        }

        // Draft HF Repo
        {
            char buf[256];
            strncpy(buf, model.hf_repo_draft.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            if (ImGui::InputText(TR("model.draft.hf_repo"), buf, sizeof(buf))) {
                model.hf_repo_draft = buf;
                modified_ = true;
            }
            InputTextContextMenu();
            ImGui::SameLine();
            HelpMarker(TR("model.draft.hf_repo.help"));
        }

        // Draft parameters
        ImGui::Text(TR("model.draft.params"));

        // Draft max
        {
            int draft_max = model.draft_max;
            if (ImGui::SliderInt(TR("model.draft.max"), &draft_max, 1, 16)) {
                model.draft_max = draft_max;
                modified_ = true;
            }
            ImGui::SameLine();
            HelpMarker(TR("model.draft.max.help"));
        }

        // Draft min
        {
            int draft_min = model.draft_min;
            if (ImGui::SliderInt(TR("model.draft.min"), &draft_min, 0, 16)) {
                model.draft_min = draft_min;
                modified_ = true;
            }
            ImGui::SameLine();
            HelpMarker(TR("model.draft.min.help"));
        }

        // Draft p_min
        {
            float draft_p_min = model.draft_p_min;
            if (ImGui::SliderFloat(TR("model.draft.p_min"), &draft_p_min, 0.0f, 1.0f, "%.2f")) {
                model.draft_p_min = draft_p_min;
                modified_ = true;
            }
            ImGui::SameLine();
            HelpMarker(TR("model.draft.p_min.help"));
        }

        ImGui::Unindent();
    }

    // =========================================================================
    // LoRA Section
    // =========================================================================
    ImGui::Separator();
    if (ImGui::CollapsingHeader("LoRA")) {
        ImGui::Indent();

        // LoRA base
        {
            char buf[512];
            strncpy(buf, model.lora_base.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            if (ImGui::InputText(TR("model.lora_base"), buf, sizeof(buf))) {
                model.lora_base = buf;
                modified_ = true;
            }
            InputTextContextMenu();
            ImGui::SameLine();
            HelpMarker(TR("model.lora_base.help"));
        }

        ImGui::Text(TR("model.lora.active"));
        // TODO: List active LoRA adapters

        ImGui::Text(TR("model.lora.add"));
        ImGui::InputTextWithHint("##new_lora_path", TR("model.lora.path"), new_lora_path_, sizeof(new_lora_path_));
        InputTextContextMenu();
        ImGui::SameLine();
        
        float lora_scale = 1.0f;
        ImGui::SliderFloat(TR("model.lora.scale"), &lora_scale, 0.1f, 2.0f, "%.1f");
        ImGui::SameLine();
        
        if (ImGui::Button(TR("button.add"))) {
            // TODO: Add LoRA adapter
        }

        ImGui::Unindent();
    }

    // =========================================================================
    // Multimodal Section
    // =========================================================================
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Multimodal")) {
        ImGui::Indent();

        // MMProj path
        {
            char buf[512];
            strncpy(buf, model.mmproj.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            if (ImGui::InputText(TR("model.mmproj.path"), buf, sizeof(buf))) {
                model.mmproj = buf;
                modified_ = true;
            }
            InputTextContextMenu();
            ImGui::SameLine();
            HelpMarker(TR("model.mmproj.path.help"));
        }

        // MMProj URL
        {
            char buf[512];
            strncpy(buf, model.mmproj_url.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            if (ImGui::InputText(TR("model.mmproj.url"), buf, sizeof(buf))) {
                model.mmproj_url = buf;
                modified_ = true;
            }
            InputTextContextMenu();
            ImGui::SameLine();
            HelpMarker(TR("model.mmproj.url.help"));
        }

        ImGui::Checkbox(TR("model.mmproj.disable"), &model.no_mmproj);
        ImGui::SameLine();
        HelpMarker(TR("model.mmproj.disable.help"));

        ImGui::Checkbox(TR("model.no_mmproj_offload"), &model.no_mmproj_offload);
        ImGui::SameLine();
        HelpMarker(TR("model.mmproj.no_offload.help"));

        ImGui::Unindent();
    }

    // =========================================================================
    // Advanced Options
    // =========================================================================
    ImGui::Separator();
    if (ImGui::CollapsingHeader(TR("model.advanced"))) {
        ImGui::Indent();

        ImGui::Checkbox(TR("model.check_tensors"), &model.check_tensors);
        ImGui::SameLine();
        HelpMarker(TR("model.check_tensors.help"));

        ImGui::Checkbox(TR("model.list_devices"), &model.list_devices);
        ImGui::SameLine();
        HelpMarker(TR("model.list_devices.help"));

        // Device
        {
            char buf[256];
            strncpy(buf, model.device.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            if (ImGui::InputText(TR("model.device"), buf, sizeof(buf))) {
                model.device = buf;
                modified_ = true;
            }
            InputTextContextMenu();
            ImGui::SameLine();
            HelpMarker(TR("model.device.help"));
        }

        // Device draft
        {
            char buf[256];
            strncpy(buf, model.device_draft.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            if (ImGui::InputText(TR("model.device_draft"), buf, sizeof(buf))) {
                model.device_draft = buf;
                modified_ = true;
            }
            InputTextContextMenu();
            ImGui::SameLine();
            HelpMarker(TR("model.device_draft.help"));
        }

        ImGui::Unindent();
    }
}

} // namespace ui
} // namespace llama_gui
