#include "../include/ui/quick_settings_dialog.h"
#include "../include/ui/localization_manager.h"
#include "../external/imgui/imgui.h"
#include <iostream>
#include "../include/ui/input_text_context_menu.h"

namespace llama_gui {
namespace ui {

QuickSettingsDialog::QuickSettingsDialog(Settings& settings)
    : settings_(settings) {
    current_profile_name_[0] = '\0';
}

QuickSettingsDialog::~QuickSettingsDialog() = default;

void QuickSettingsDialog::HelpMarker(const std::string& desc) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void QuickSettingsDialog::render() {
    if (!show_dialog_) return;

    ImGui::OpenPopup("Quick Settings");

    if (ImGui::BeginPopupModal("Quick Settings", &show_dialog_, ImGuiWindowFlags_AlwaysAutoResize)) {
        
        // Заголовок
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Quick Settings");
        ImGui::TextDisabled("Frequently used settings");
        ImGui::Separator();

        if (ImGui::BeginTabBar("QuickSettingsTabs")) {
            
            // =========================================================================
            // Server Tab
            // =========================================================================
            if (ImGui::BeginTabItem("Server")) {
                render_server_tab();
                ImGui::EndTabItem();
            }

            // =========================================================================
            // Chat Tab
            // =========================================================================
            if (ImGui::BeginTabItem("Chat")) {
                render_chat_tab();
                ImGui::EndTabItem();
            }

            // =========================================================================
            // Models Tab
            // =========================================================================
            if (ImGui::BeginTabItem("Models")) {
                render_models_tab();
                ImGui::EndTabItem();
            }

            // =========================================================================
            // UI Tab
            // =========================================================================
            if (ImGui::BeginTabItem("UI")) {
                render_ui_tab();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::Separator();

        // Кнопки управления
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

        ImGui::EndPopup();
    }
}

void QuickSettingsDialog::render_server_tab() {
    ImGui::Text("Server Configuration");
    ImGui::Separator();

    auto& server = settings_.server();

    char host_buf[256];
    strncpy(host_buf, server.host.c_str(), sizeof(host_buf) - 1);
    host_buf[sizeof(host_buf) - 1] = '\0';

    if (ImGui::InputText("Host", host_buf, sizeof(host_buf))) {
        server.host = host_buf;
        settings_modified_ = true;
    }
    InputTextContextMenu();

    int port = server.port;
    if (ImGui::InputInt("Port", &port)) {
        server.port = port;
        settings_modified_ = true;
    }

    ImGui::Separator();

    // API URL (read-only)
    std::string api_url = "http://" + server.host + ":" + std::to_string(server.port);
    ImGui::Text("API URL: %s", api_url.c_str());

    ImGui::Separator();

    // Кнопки управления сервером
    ImGui::Text("Server Control:");
    if (ImGui::Button("Start Server")) {
        std::cout << "QuickSettings: Start Server requested" << std::endl;
        if (start_server_callback_) start_server_callback_();
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop Server")) {
        std::cout << "QuickSettings: Stop Server requested" << std::endl;
        if (stop_server_callback_) stop_server_callback_();
    }
    ImGui::SameLine();
    if (ImGui::Button("Restart")) {
        std::cout << "QuickSettings: Restart Server requested" << std::endl;
        if (restart_server_callback_) restart_server_callback_();
    }

    // Статус сервера
    ImGui::Separator();
    ImGui::Text("Server Status: ");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "Stopped"); // Заглушка
}

void QuickSettingsDialog::render_chat_tab() {
    ImGui::Text("Chat Configuration");
    ImGui::Separator();

    auto& chat = settings_.chat();

    // System Prompt
    static char system_prompt_buf[1024];
    strncpy(system_prompt_buf, chat.default_system_prompt.c_str(), sizeof(system_prompt_buf) - 1);
    system_prompt_buf[sizeof(system_prompt_buf) - 1] = '\0';

    if (ImGui::InputTextMultiline("System Prompt", system_prompt_buf, sizeof(system_prompt_buf), 
                                   ImVec2(-FLT_MIN, 80.0f))) {
        chat.default_system_prompt = system_prompt_buf;
        settings_modified_ = true;
    }
    InputTextContextMenu();

    ImGui::Separator();

    // Generation Parameters
    ImGui::Text("Generation Parameters:");
    
    if (ImGui::InputInt("Max Tokens", &chat.max_tokens)) {
        settings_modified_ = true;
    }
    HelpMarker("Maximum number of tokens to generate");

    float temp = chat.temperature;
    if (ImGui::SliderFloat("Temperature", &temp, 0.0f, 2.0f, "%.2f")) {
        chat.temperature = temp;
        settings_modified_ = true;
    }
    HelpMarker("Higher values = more random, lower values = more deterministic");

    float top_p = chat.top_p;
    if (ImGui::SliderFloat("Top P", &top_p, 0.0f, 1.0f, "%.2f")) {
        chat.top_p = top_p;
        settings_modified_ = true;
    }
    HelpMarker("Nucleus sampling: consider top tokens with cumulative probability <= P");

    int top_k = chat.top_k;
    if (ImGui::SliderInt("Top K", &top_k, 1, 100)) {
        chat.top_k = top_k;
        settings_modified_ = true;
    }
    HelpMarker("Consider only top K tokens");

    float repeat_penalty = chat.repeat_penalty;
    if (ImGui::SliderFloat("Repeat Penalty", &repeat_penalty, 1.0f, 2.0f, "%.2f")) {
        chat.repeat_penalty = repeat_penalty;
        settings_modified_ = true;
    }
    HelpMarker("Penalty for repeating tokens");

    ImGui::Separator();

    // CPU Threads
    if (ImGui::InputInt("CPU Threads", &chat.threads)) {
        settings_modified_ = true;
    }
    HelpMarker("Number of CPU threads to use");

    // Context Size
    int n_ctx = chat.n_ctx;
    if (ImGui::InputInt("Context Size", &n_ctx)) {
        chat.n_ctx = n_ctx;
        settings_modified_ = true;
    }
    HelpMarker("Context window size (n_ctx)");

    // GPU Layers
    if (ImGui::InputInt("GPU Layers", &chat.n_gpu_layers)) {
        settings_modified_ = true;
    }
    HelpMarker("Number of layers to offload to GPU");
}

void QuickSettingsDialog::render_models_tab() {
    ImGui::Text("Model Configuration");
    ImGui::Separator();

    // Model path
    std::string model_path = settings_.get_model_path();
    static char model_path_buf[512];
    strncpy(model_path_buf, model_path.c_str(), sizeof(model_path_buf) - 1);
    model_path_buf[sizeof(model_path_buf) - 1] = '\0';

    if (ImGui::InputText("Model Path", model_path_buf, sizeof(model_path_buf))) {
        settings_.set_model_path(model_path_buf);
        settings_modified_ = true;
    }
    InputTextContextMenu();
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        if (browse_callback_) {
            browse_callback_([this](const std::string& path) {
                if (!path.empty()) {
                    strncpy(model_path_buf, path.c_str(), sizeof(model_path_buf) - 1);
                    model_path_buf[sizeof(model_path_buf) - 1] = '\0';
                    settings_.set_model_path(path);
                    settings_modified_ = true;
                }
            });
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply Model") && model_changed_callback_) {
        model_changed_callback_();
    }

    ImGui::Separator();

    // Model directory
    ImGui::Text("Model Directory:");
    if (ImGui::Button("Set Model Directory")) {
        // TODO: Open directory dialog
        std::cout << "QuickSettings: Set model directory" << std::endl;
    }

    ImGui::Separator();

    // Embedding model
    std::string emb_model_path = settings_.get_embedding_model_path();
    static char emb_model_buf[512];
    strncpy(emb_model_buf, emb_model_path.c_str(), sizeof(emb_model_buf) - 1);
    emb_model_buf[sizeof(emb_model_buf) - 1] = '\0';

    if (ImGui::InputText("Embedding Model (optional)", emb_model_buf, sizeof(emb_model_buf))) {
        settings_.set_embedding_model_path(emb_model_buf);
        settings_modified_ = true;
    }
    InputTextContextMenu();
}

void QuickSettingsDialog::render_ui_tab() {
    ImGui::Text("User Interface Settings");
    ImGui::Separator();

    auto& display = settings_.display();

    // Theme
    const char* themes[] = {"Dark", "Light", "Auto"};
    int current_theme = settings_.is_dark_theme() ? 0 : 1;
    
    if (ImGui::Combo("Theme", &current_theme, themes, 3)) {
        settings_.set_theme(static_cast<llama_gui::core::ThemeType>(current_theme));
        settings_modified_ = true;
    }

    // Font size
    float font_size = display.font_size;
    if (ImGui::SliderFloat("Font Size", &font_size, 10.0f, 24.0f, "%.1f")) {
        display.font_size = font_size;
        settings_modified_ = true;
    }

    ImGui::Separator();

    // Window size
    ImGui::Text("Window Size:");
    int width = display.window_width;
    int height = display.window_height;
    
    if (ImGui::InputInt("Width", &width)) {
        display.window_width = width;
        settings_modified_ = true;
    }
    ImGui::SameLine();
    if (ImGui::InputInt("Height", &height)) {
        display.window_height = height;
        settings_modified_ = true;
    }

    if (ImGui::Checkbox("Maximized", &display.window_maximized)) {
        settings_modified_ = true;
    }

    ImGui::Separator();

    // Performance
    if (ImGui::Checkbox("V-Sync", &settings_.performance().enable_vsync)) {
        settings_modified_ = true;
    }
    HelpMarker("Enable vertical sync");

    int target_fps = settings_.performance().target_fps;
    if (ImGui::SliderInt("FPS Limit", &target_fps, 15, 144)) {
        settings_.performance().target_fps = target_fps;
        settings_modified_ = true;
    }
}

void QuickSettingsDialog::save_settings() {
    std::cout << "QuickSettingsDialog: Saving settings" << std::endl;

    // Use settings_.save_profile directly (old API — still works)
    std::string profile = settings_.get_current_profile_name();
    if (!profile.empty()) {
        if (settings_.save_profile(profile)) {
            status_message_ = "Settings saved to: " + profile;
        } else {
            status_message_ = "Failed to save";
        }
    } else {
        status_message_ = "No profile loaded";
    }
}

void QuickSettingsDialog::apply_settings() {
    std::cout << "QuickSettingsDialog: Applying settings" << std::endl;
    save_settings();
    status_message_ = "Settings applied and saved";
}

void QuickSettingsDialog::reset_settings() {
    std::cout << "QuickSettingsDialog: Resetting settings" << std::endl;
    settings_.reset_to_defaults();
    status_message_ = "Settings reset to defaults";
}

void QuickSettingsDialog::cancel_settings() {
    hide();
}

} // namespace ui
} // namespace llama_gui
