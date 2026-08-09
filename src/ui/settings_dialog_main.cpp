#include "../include/ui/settings_dialog.h"
#include "../include/ui/settings_dialog_model.h"
#include "../include/ui/settings_dialog_gpu.h"
#include "../include/ui/settings_dialog_sampling_basic.h"
#include "../include/ui/settings_dialog_sampling_advanced.h"
#include "../include/ui/settings_dialog_context.h"
#include "../include/ui/settings_dialog_rope.h"
#include "../include/ui/settings_dialog_advanced.h"
#include "../include/ui/settings_dialog_grammar.h"
#include "../include/ui/settings_dialog_server_runtime.h"
#include "../include/ui/settings_dialog_batch.h"
#include "../include/ui/settings_dialog_logging.h"
#include "../include/ui/quick_settings_dialog.h"
#include "../include/ui/advanced_settings_dialog.h"
#include "../include/ui/localization_manager.h"
#include "../include/core/logger.h"
#include "../include/core/config_manager.h"
#include "../external/imgui/imgui.h"
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <thread>
#include <chrono>

namespace llama_gui {
namespace ui {

// Helper function for tooltips with Russian text
void SettingsDialog::HelpMarker(const std::string& desc) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

SettingsDialog::SettingsDialog(Settings& settings, ServerManager* server_manager, WorkspaceManager* workspace_manager, llama_gui::core::ConfigManager* config_manager)
    : settings_(settings)
    , server_manager_(server_manager)
    , workspace_manager_(workspace_manager)
    , config_manager_(config_manager) {

    // Initialize system prompt settings module
    system_prompt_settings_ = std::make_unique<SystemPromptSettings>(settings_);

    // =========================================================================
    // Инициализация новых диалогов (двухуровневая система)
    // =========================================================================

    // Диалог быстрых настроек
    quick_settings_dialog_ = std::make_unique<QuickSettingsDialog>(settings_);

    // Диалог расширенных настроек
    advanced_settings_dialog_ = std::make_unique<AdvancedSettingsDialog>(settings_);

    // =========================================================================
    // Старые диалоги (для обратной совместимости)
    // =========================================================================

    // Initialize model settings dialog
    model_settings_dialog_ = std::make_unique<ModelSettingsDialog>(settings_);

    // Initialize GPU settings dialog
    gpu_settings_dialog_ = std::make_unique<GPUSettingsDialog>(settings_);

    // Initialize sampling settings dialogs
    sampling_basic_dialog_ = std::make_unique<SamplingBasicDialog>(settings_);
    sampling_advanced_dialog_ = std::make_unique<SamplingAdvancedDialog>(settings_);

    // Initialize context and RoPE settings dialogs
    context_dialog_ = std::make_unique<ContextDialog>(settings_);
    rope_dialog_ = std::make_unique<RoPEDialog>(settings_);

    // Initialize Advanced and Grammar settings dialogs
    advanced_dialog_ = std::make_unique<AdvancedDialog>(settings_);
    grammar_dialog_ = std::make_unique<GrammarDialog>(settings_);

    // Initialize Server Runtime, Batch and Logging settings dialogs
    server_runtime_dialog_ = std::make_unique<ServerRuntimeDialog>(settings_);
    batch_dialog_ = std::make_unique<BatchDialog>(settings_);
    logging_dialog_ = std::make_unique<LoggingDialog>(settings_);
}

// Деструктор должен быть после всех include для корректного уничтожения unique_ptr
SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::setModelBrowseCallback(std::function<void(std::function<void(const std::string&)>)> cb) {
    if (model_settings_dialog_) {
        model_settings_dialog_->setBrowseCallback(std::move(cb));
    }
}

void SettingsDialog::render() {
    if (!show_dialog_) return;

    ImGui::OpenPopup("Settings");

    // Стартовый размер при первом открытии и минимальные границы — иначе в
    // расширенном режиме попап открывается маленьким, и элементы обрезаются.
    ImGui::SetNextWindowSize(ImVec2(700, 480), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(540, 380), ImVec2(FLT_MAX, FLT_MAX));

    if (ImGui::BeginPopupModal("Settings", &show_dialog_, show_quick_ ? ImGuiWindowFlags_AlwaysAutoResize : ImGuiWindowFlags_None)) {

        // Переключатель между быстрыми и расширенными настройками
        // Скрываем для User режима - оставляем только быстрые настройки
        bool is_user_mode = workspace_manager_ && 
                           workspace_manager_->getCurrentWorkspaceType() == WorkspaceType::User;

        if (!is_user_mode) {
            // Показываем переключатель только для Developer и Admin
            ImGui::Text("%s", TR("settings.mode"));
            ImGui::SameLine();

            if (ImGui::Button(show_quick_ ? TR("settings.quick") : TR("settings.advanced"))) {
                show_quick_ = !show_quick_;
            }

            ImGui::SameLine();
            ImGui::TextDisabled(show_quick_
                ? TR("settings.frequently_used")
                : TR("settings.detailed"));
        }
        // Для User режима не показываем заголовок - он будет в секции ниже

        ImGui::Separator();

        if (show_quick_) {
            // Рендерим быстрые настройки - показываем только одну секцию за раз
            // В режиме пользователя не показываем заголовок секции
            if (!is_user_mode) {
                ImGui::Text(TR("settings.quick"));
                ImGui::Separator();
            }
            
            switch (current_tab_) {
                case SettingsTab::Server:
                    render_server_settings();
                    break;
                case SettingsTab::Chat:
                    render_chat_settings();
                    break;
                case SettingsTab::Models:
                    render_model_settings();
                    break;
                case SettingsTab::UI:
                    render_ui_settings();
                    break;
                default:
                    render_server_settings();
                    break;
            }
        } else {
            // Рендерим расширенные настройки через AdvancedSettingsDialog
            if (advanced_settings_dialog_) {
                advanced_settings_dialog_->render();
            }
        }

        ImGui::Separator();

        // Buttons
        if (ImGui::Button(TR("button.save"))) {
            save_settings();
            // Check for model change even if settings_modified_ wasn't set
            // (ModelSettingsDialog has its own modified_ flag)
            check_model_changed();
            hide();
        }
        ImGui::SameLine();
        if (ImGui::Button(TR("button.apply"))) {
            apply_settings();
        }
        ImGui::SameLine();
        if (ImGui::Button(TR("button.reset"))) {
            reset_settings();
        }
        ImGui::SameLine();
        if (ImGui::Button(TR("button.cancel"))) {
            cancel_settings();
        }

        ImGui::EndPopup();
    }
}

void SettingsDialog::save_settings() {
    std::cout << "SettingsDialog: Saving settings" << std::endl;

    // Apply system prompt settings before saving
    system_prompt_settings_->apply_settings();

    // Сохраняем в текущий загруженный профиль через ProfileManager
    std::string profile_name;
    if (config_manager_) {
        profile_name = config_manager_->getCurrentProfileName();
        if (!profile_name.empty() && config_manager_->saveProfile(profile_name)) {
            status_message_ = TRF("profiles.saved_to", profile_name.c_str());
            std::cout << "Settings saved to profile: " << profile_name << std::endl;
            return;
        }
    }

    status_message_ = TR("profiles.error_no_loaded");
    std::cerr << "No profile loaded to save settings" << std::endl;
}

void SettingsDialog::reset_settings() {
    std::cout << "SettingsDialog: Resetting settings" << std::endl;
    settings_.reset_to_defaults();

    // Also reset system prompt settings to default
    system_prompt_settings_->reset_to_default();
}

void SettingsDialog::apply_settings() {
    std::cout << "SettingsDialog: Applying settings" << std::endl;

    // Сохраняем настройки
    save_settings();

    // Check for model change and trigger server restart if needed
    check_model_changed();

    settings_modified_ = false;
    status_message_ = "Настройки применены";
}

void SettingsDialog::cancel_settings() {
    restore_backup();
    // Also restore system prompt settings from backup
    system_prompt_settings_->restore_backup();
    hide();
}

void SettingsDialog::check_model_changed() {
    std::string current_model_path = settings_.get_model_path();
    if (model_changed_callback_ && !current_model_path.empty() && current_model_path != saved_model_path_) {
        std::cout << "SettingsDialog: Model changed from '" << saved_model_path_
                  << "' to '" << current_model_path << "', triggering server restart" << std::endl;
        model_changed_callback_(current_model_path);
        saved_model_path_ = current_model_path;
    }
}

void SettingsDialog::backup_current_settings() {
    // Create a backup by serializing and deserializing
    backup_json_ = settings_.serialize_to_json();

    // Also backup system prompt settings
    system_prompt_settings_->backup_current_settings();
}

void SettingsDialog::restore_backup() {
    if (!backup_json_.empty()) {
        settings_.deserialize_from_json(backup_json_);
    }

    // Also restore system prompt settings from backup
    system_prompt_settings_->restore_backup();
}

} // namespace ui
} // namespace llama_gui
