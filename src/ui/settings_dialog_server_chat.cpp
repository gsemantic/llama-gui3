#include "../include/ui/settings_dialog.h"
#include "../include/ui/localization_manager.h"
#include "../external/imgui/imgui.h"
#include <cstring>
#include <iostream>
#include <vector>
#include <algorithm>
#include "../include/ui/input_text_context_menu.h"

namespace llama_gui {
namespace ui {

void SettingsDialog::render_server_settings() {
    auto& server_settings = settings_.server();

    ImGui::Text(TR("server.configuration"));
    ImGui::Separator();

    // Server URL
    char url_buffer[256];
    strcpy(url_buffer, server_settings.api_url.c_str());
    if (ImGui::InputText(TR("server.url"), url_buffer, sizeof(url_buffer))) {
        server_settings.api_url = url_buffer;
    }
    InputTextContextMenu();

    // Timeout
    ImGui::InputInt(TR("server.timeout"), &server_settings.connection_timeout);

    // Max retries
    ImGui::InputInt(TR("server.max_retries"), &server_settings.max_retries);

    // SSL
    ImGui::Checkbox(TR("server.use_ssl"), &server_settings.verify_ssl);

    ImGui::Separator();
    
    // =========================================================================
    // OpenRouter / Cloud Services
    // Настройки перенесены в отдельный диалог "Облачные сервисы"
    // =========================================================================
    ImGui::Text("Cloud Provider / Облачный провайдер");
    ImGui::Separator();
    
    ImGui::Text("Настройки облачных провайдеров (OpenAI-совместимые) доступны в отдельном окне.");
    ImGui::Spacing();
    
    auto& cp = settings_.cloud_provider();
    
    // Индикатор текущего состояния
    ImGui::Text("Текущее состояние:");
    ImGui::SameLine();
    if (cp.enabled && !cp.model_id.empty()) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Cloud active: %s", cp.model_id.c_str());
    } else if (cp.enabled) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Cloud provider enabled, no model selected");
    } else {
        ImGui::Text("Using local server");
    }
    
    ImGui::Spacing();
    
    // Кнопка для открытия диалога облачных сервисов
    if (ImGui::Button("Открыть настройки облачных сервисов...", ImVec2(250, 0))) {
        if (cloud_services_dialog_) {
            cloud_services_dialog_->open();
        }
    }
    ImGui::SameLine();
    HelpMarker("Открыть диалог настройки облачных сервисов (OpenRouter API, выбор модели, статистика использования)");
    
    ImGui::Separator();
    ImGui::Text(TR("server.control"));

    if (ImGui::Button(TR("server.restart_with_new_model"))) {
        if (server_manager_) {
            // Update model path from settings before restarting
            std::string model_path = settings_.get_model_path();
            std::cout << "SettingsDialog: Restarting server with model path: " << model_path << std::endl;

            // Sync model path to server manager so build_server_command uses the new model
            server_manager_->set_model_path(model_path);

            // Restart server with current settings
            server_manager_->restart_server();
            std::cout << "Server restart initiated" << std::endl;

            // Notify MainWindow to reconnect LlamaInterface
            if (server_started_callback_) {
                std::cout << "SettingsDialog: Notifying about server started" << std::endl;
                server_started_callback_();
            }
        }
    }

    ImGui::SameLine();
    if (ImGui::Button(TR("server.check_status"))) {
        if (server_manager_) {
            bool is_running = server_manager_->is_server_running();
            std::string status = server_manager_->get_server_status();
            std::cout << "Server status: " << (is_running ? "Running" : "Stopped") << " - " << status << std::endl;
        }
    }

    // Show server status
    if (server_manager_) {
        ImGui::Text(TR("server.status"), server_manager_->is_server_running() ? TR("server.running") : TR("server.stopped"));
        ImGui::Text(TR("server.details"), server_manager_->get_server_status().c_str());
    }
}

void SettingsDialog::render_chat_settings() {
    auto& chat_settings = settings_.chat();

    ImGui::Text(TR("chat.generation"));
    ImGui::Separator();

    // Temperature
    ImGui::SliderFloat(TR("chat.temperature"), &chat_settings.temperature, 0.0f, 2.0f);
    ImGui::SameLine();
    HelpMarker(TR("chat.temperature.help"));

    // Top P (Nucleus Sampling)
    ImGui::SliderFloat(TR("chat.top_p"), &chat_settings.top_p, 0.0f, 1.0f);
    ImGui::SameLine();
    HelpMarker(TR("chat.top_p.help"));

    // Top K
    ImGui::InputInt(TR("chat.top_k"), &chat_settings.top_k);
    ImGui::SameLine();
    HelpMarker(TR("chat.top_k.help"));

    // Min P
    ImGui::SliderFloat(TR("chat.min_p"), &chat_settings.min_p, 0.0f, 1.0f);
    ImGui::SameLine();
    HelpMarker(TR("chat.min_p.help"));

    // Repeat Penalty
    ImGui::SliderFloat(TR("chat.repeat_penalty"), &chat_settings.repeat_penalty, 1.0f, 2.0f);
    ImGui::SameLine();
    HelpMarker(TR("chat.repeat_penalty.help"));

    // Presence Penalty
    ImGui::SliderFloat(TR("chat.presence_penalty"), &chat_settings.presence_penalty, -2.0f, 2.0f);
    ImGui::SameLine();
    HelpMarker(TR("chat.presence_penalty.help"));

    // Frequency Penalty
    ImGui::SliderFloat(TR("chat.frequency_penalty"), &chat_settings.frequency_penalty, -2.0f, 2.0f);
    ImGui::SameLine();
    HelpMarker(TR("chat.frequency_penalty.help"));

    ImGui::Separator();
    ImGui::Text(TR("mirostat.title"));

    // Mirostat Mode
    const char* mirostat_modes[] = { TR("general.none"), "Mirostat 1.0", "Mirostat 2.0" };
    ImGui::Combo(TR("mirostat.mode"), &chat_settings.mirostat_mode, mirostat_modes, IM_ARRAYSIZE(mirostat_modes));
    ImGui::SameLine();
    HelpMarker(TR("mirostat.mode.help"));

    if (chat_settings.mirostat_mode > 0) {
        ImGui::SliderFloat(TR("mirostat.tau"), &chat_settings.mirostat_tau, 0.0f, 10.0f);
        ImGui::SameLine();
        HelpMarker(TR("mirostat.tau.help"));

        ImGui::SliderFloat(TR("mirostat.eta"), &chat_settings.mirostat_eta, 0.0f, 1.0f);
        ImGui::SameLine();
        HelpMarker(TR("mirostat.eta.help"));
    }

    ImGui::Separator();
    ImGui::Text(TR("context.limits"));

    // Max Tokens
    // ПРИМЕЧАНИЕ: Это значение синхронизируется с output.n_predict при загрузке профиля
    // (если включена настройка sync_max_tokens_enabled). Синхронизация обеспечивает
    // согласованность между параметрами API запросов (chat.max_tokens) и командной строки
    // сервера (output.n_predict).
    int max_tokens = chat_settings.max_tokens;
    if (ImGui::InputInt(TR("chat.max_tokens"), &max_tokens)) {
        chat_settings.max_tokens = max_tokens;
    }
    ImGui::SameLine();
    HelpMarker(TR("chat.max_tokens.help"));

    // Валидация max_tokens относительно ctx_size
    if (!settings_.validate_max_tokens()) {
        std::string error = settings_.get_max_tokens_validation_error();
        int recommended = settings_.get_recommended_max_tokens();
        int max_allowed = settings_.get_max_allowed_tokens();
        int ctx_size = settings_.batch().ctx_size;

        // Показываем предупреждение красным цветом
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        ImGui::TextWrapped("[!] %s", error.c_str());
        ImGui::PopStyleColor();

        // HelpMarker с рекомендацией
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
            ImGui::Text("Рекомендуется не более %d токенов при контексте %d", recommended, ctx_size);
            ImGui::Text("Максимально допустимо: %d токенов", max_allowed);
            ImGui::Text("Запас на промпт/историю: %d токенов (20%%)", settings_.get_prompt_reserve());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    } else {
        // Показываем рекомендацию в обычном режиме
        int recommended = settings_.get_recommended_max_tokens();
        int ctx_size = settings_.batch().ctx_size;
        
        ImGui::SameLine();
        ImGui::TextDisabled("[~%d rec.]", recommended);
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
            ImGui::Text("Рекомендуется не более %d токенов при контексте %d", recommended, ctx_size);
            ImGui::Text("Запас на промпт/историю: %d токенов (20%%)", settings_.get_prompt_reserve());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    // Stop on newline
    ImGui::Checkbox(TR("chat.stop_on_newline"), &chat_settings.stop_on_newline);
    ImGui::SameLine();
    HelpMarker(TR("chat.stop_on_newline.help"));
}

} // namespace ui
} // namespace llama_gui
