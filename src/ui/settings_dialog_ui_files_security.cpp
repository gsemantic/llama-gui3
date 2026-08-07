#include "../include/ui/settings_dialog.h"
#include "../include/ui/localization_manager.h"
#include "../include/ui/language_selector.h"
#include "../external/imgui/imgui.h"
#include <cstring>

namespace llama_gui {
namespace ui {

void SettingsDialog::render_ui_settings() {
    auto& display = settings_.display();
    auto& perf = settings_.performance();

    // === Язык интерфейса ===
    if (ImGui::CollapsingHeader(TRF("settings.language", "Language"), ImGuiTreeNodeFlags_DefaultOpen)) {
        LanguageSelector selector;
        selector.renderComboBox(TRF("settings.language.interface", "Interface Language"));
        if (selector.languageChanged()) {
            settings_modified_ = true;
        }
    }

    // === Тема ===
    if (ImGui::CollapsingHeader(TRF("settings.ui.theme", "Theme"), ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* themes[] = {
            TRF("settings.ui.theme.dark", "Dark"),
            TRF("settings.ui.theme.light", "Light"),
            TRF("settings.ui.theme.auto", "Auto")
        };
        int current_theme = settings_.is_dark_theme() ? 0 : (settings_.get_theme() == core::ThemeType::Light ? 1 : 2);
        if (ImGui::Combo(TRF("settings.ui.theme.appearance", "Appearance Theme"), &current_theme, themes, 3)) {
            settings_.set_theme(static_cast<core::ThemeType>(current_theme));
            settings_modified_ = true;
        }
    }

    // === Шрифт ===
    if (ImGui::CollapsingHeader(TRF("settings.ui.font", "Font"), ImGuiTreeNodeFlags_DefaultOpen)) {
        float font_size = display.font_size;
        if (ImGui::SliderFloat(TRF("settings.label.font_size", "Font Size"), &font_size, 10.0f, 28.0f, "%.1f")) {
            display.font_size = font_size;
            settings_modified_ = true;
        }
    }

    // === Окно ===
    if (ImGui::CollapsingHeader(TRF("settings.ui.window", "Window"), ImGuiTreeNodeFlags_DefaultOpen)) {
        int width = display.window_width;
        int height = display.window_height;
        if (ImGui::InputInt(TRF("settings.ui.window.width", "Width"), &width)) {
            display.window_width = width;
            settings_modified_ = true;
        }
        if (ImGui::InputInt(TRF("settings.ui.window.height", "Height"), &height)) {
            display.window_height = height;
            settings_modified_ = true;
        }

        if (ImGui::Checkbox(TRF("settings.ui.window.maximized", "Maximized"), &display.window_maximized)) {
            settings_modified_ = true;
        }

        if (ImGui::Checkbox(TRF("settings.ui.window.auto_resize", "Auto Resize"), &display.auto_resize)) {
            settings_modified_ = true;
        }
        HelpMarker(TRF("settings.ui.window.auto_resize.help", "Automatically adjust the window size to the monitor resolution"));

        if (ImGui::Checkbox(TRF("settings.ui.window.center", "Center Window"), &display.center_window)) {
            settings_modified_ = true;
        }

        int min_w = display.min_window_width;
        int min_h = display.min_window_height;
        if (ImGui::InputInt(TRF("settings.ui.window.min_width", "Min Width"), &min_w)) {
            display.min_window_width = min_w;
            settings_modified_ = true;
        }
        if (ImGui::InputInt(TRF("settings.ui.window.min_height", "Min Height"), &min_h)) {
            display.min_window_height = min_h;
            settings_modified_ = true;
        }
    }

    // === Анимация и производительность ===
    if (ImGui::CollapsingHeader(TRF("settings.ui.performance", "Interface Performance"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox(TRF("settings.ui.animations", "Animations"), &display.enable_animation)) {
            settings_modified_ = true;
        }

        if (ImGui::Checkbox(TR("settings.label.vsync"), &perf.enable_vsync)) {
            settings_modified_ = true;
        }
        HelpMarker(TRF("settings.ui.vsync.help", "Vertical sync — eliminates screen tearing"));

        int target_fps = perf.target_fps;
        if (ImGui::SliderInt(TRF("settings.label.fps_limit", "FPS Limit"), &target_fps, 15, 240)) {
            perf.target_fps = target_fps;
            settings_modified_ = true;
        }

        int idle_fps = perf.idle_fps;
        if (ImGui::SliderInt(TRF("settings.ui.idle_fps", "Idle FPS"), &idle_fps, 5, 60)) {
            perf.idle_fps = idle_fps;
            settings_modified_ = true;
        }
        HelpMarker(TRF("settings.ui.idle_fps.help", "Frame rate when the window is not active"));

        int idle_timeout = perf.idle_timeout_ms;
        if (ImGui::SliderInt(TRF("settings.ui.idle_timeout", "Idle Timeout (ms)"), &idle_timeout, 1000, 30000)) {
            perf.idle_timeout_ms = idle_timeout;
            settings_modified_ = true;
        }
        HelpMarker(TRF("settings.ui.idle_timeout.help", "Idle time before switching to reduced FPS mode"));

        if (ImGui::Checkbox(TRF("settings.ui.smart_redraw", "Smart Redraw"), &perf.enable_smart_redraw)) {
            settings_modified_ = true;
        }
        HelpMarker(TRF("settings.ui.smart_redraw.help", "Redraw only on changes (saves CPU)"));

        if (ImGui::Checkbox(TRF("settings.ui.performance_overlay", "Show Performance Overlay"), &perf.show_performance_overlay)) {
            settings_modified_ = true;
        }
    }
}

void SettingsDialog::render_file_settings() {
    auto& files = settings_.files();

    if (ImGui::CollapsingHeader(TRF("settings.files.default_paths", "Default Paths"), ImGuiTreeNodeFlags_DefaultOpen)) {
        // Default save path
        char save_path[512];
        std::strncpy(save_path, files.default_save_path.c_str(), sizeof(save_path) - 1);
        save_path[sizeof(save_path) - 1] = '\0';
        if (ImGui::InputText(TRF("settings.files.save_path", "Save Path"), save_path, sizeof(save_path))) {
            files.default_save_path = save_path;
            settings_modified_ = true;
        }

        // Auto-save path
        char auto_path[512];
        std::strncpy(auto_path, files.auto_save_path.c_str(), sizeof(auto_path) - 1);
        auto_path[sizeof(auto_path) - 1] = '\0';
        if (ImGui::InputText(TRF("settings.files.auto_save_path", "Auto-save Path"), auto_path, sizeof(auto_path))) {
            files.auto_save_path = auto_path;
            settings_modified_ = true;
        }
    }

    if (ImGui::CollapsingHeader(TRF("settings.files.auto_save", "Auto-save"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox(TRF("settings.files.auto_save_enabled", "Enable Auto-save"), &files.auto_save_enabled)) {
            settings_modified_ = true;
        }

        int interval = files.auto_save_interval;
        if (ImGui::SliderInt(TRF("settings.files.auto_save_interval", "Interval (sec)"), &interval, 30, 1800)) {
            files.auto_save_interval = interval;
            settings_modified_ = true;
        }
    }
}

void SettingsDialog::render_security_settings() {
    auto& server = settings_.server();

    if (ImGui::CollapsingHeader(TRF("settings.security.ssl", "SSL / TLS"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox(TRF("settings.security.verify_ssl", "Verify SSL Certificates"), &server.verify_ssl)) {
            settings_modified_ = true;
        }
        HelpMarker(TRF("settings.security.verify_ssl.help", "Disable only if you use self-signed certificates"));
    }

    if (ImGui::CollapsingHeader(TRF("settings.security.token", "Access Token"), ImGuiTreeNodeFlags_DefaultOpen)) {
        char token[256];
        std::strncpy(token, server.auth_token.c_str(), sizeof(token) - 1);
        token[sizeof(token) - 1] = '\0';
        if (ImGui::InputText(TRF("settings.security.token", "Access Token"), token, sizeof(token), ImGuiInputTextFlags_Password)) {
            server.auth_token = token;
            settings_modified_ = true;
        }
        HelpMarker(TRF("settings.security.token.help", "Token for server authentication (if enabled)"));
    }
}

} // namespace ui
} // namespace llama_gui
