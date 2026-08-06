#include "../include/ui/settings_dialog.h"
#include "../include/ui/localization_manager.h"
#include "../external/imgui/imgui.h"

namespace llama_gui {
namespace ui {

void SettingsDialog::render_ui_settings() {
    auto& display = settings_.display();
    auto& perf = settings_.performance();

    // === Тема ===
    if (ImGui::CollapsingHeader("Тема", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* themes[] = {"Тёмная", "Светлая", "Авто"};
        int current_theme = settings_.is_dark_theme() ? 0 : (settings_.get_theme() == core::ThemeType::Light ? 1 : 2);
        if (ImGui::Combo("Тема оформления", &current_theme, themes, 3)) {
            settings_.set_theme(static_cast<core::ThemeType>(current_theme));
            settings_modified_ = true;
        }
    }

    // === Шрифт ===
    if (ImGui::CollapsingHeader("Шрифт", ImGuiTreeNodeFlags_DefaultOpen)) {
        float font_size = display.font_size;
        if (ImGui::SliderFloat("Размер шрифта", &font_size, 10.0f, 28.0f, "%.1f")) {
            display.font_size = font_size;
            settings_modified_ = true;
        }
    }

    // === Окно ===
    if (ImGui::CollapsingHeader("Окно", ImGuiTreeNodeFlags_DefaultOpen)) {
        int width = display.window_width;
        int height = display.window_height;
        if (ImGui::InputInt("Ширина", &width)) {
            display.window_width = width;
            settings_modified_ = true;
        }
        if (ImGui::InputInt("Высота", &height)) {
            display.window_height = height;
            settings_modified_ = true;
        }

        if (ImGui::Checkbox("Развернуто", &display.window_maximized)) {
            settings_modified_ = true;
        }

        if (ImGui::Checkbox("Автоматический размер", &display.auto_resize)) {
            settings_modified_ = true;
        }
        HelpMarker("Автоматически подстраивать размер окна под разрешение монитора");

        if (ImGui::Checkbox("Центрировать окно", &display.center_window)) {
            settings_modified_ = true;
        }

        int min_w = display.min_window_width;
        int min_h = display.min_window_height;
        if (ImGui::InputInt("Мин. ширина", &min_w)) {
            display.min_window_width = min_w;
            settings_modified_ = true;
        }
        if (ImGui::InputInt("Мин. высота", &min_h)) {
            display.min_window_height = min_h;
            settings_modified_ = true;
        }
    }

    // === Анимация и производительность ===
    if (ImGui::CollapsingHeader("Производительность интерфейса", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Анимации", &display.enable_animation)) {
            settings_modified_ = true;
        }

        if (ImGui::Checkbox("V-Sync", &perf.enable_vsync)) {
            settings_modified_ = true;
        }
        HelpMarker("Вертикальная синхронизация — убирает разрывы кадров");

        int target_fps = perf.target_fps;
        if (ImGui::SliderInt("Лимит FPS", &target_fps, 15, 240)) {
            perf.target_fps = target_fps;
            settings_modified_ = true;
        }

        int idle_fps = perf.idle_fps;
        if (ImGui::SliderInt("FPS в простое", &idle_fps, 5, 60)) {
            perf.idle_fps = idle_fps;
            settings_modified_ = true;
        }
        HelpMarker("Частота кадров, когда окно не активно");

        int idle_timeout = perf.idle_timeout_ms;
        if (ImGui::SliderInt("Таймаут простоя (мс)", &idle_timeout, 1000, 30000)) {
            perf.idle_timeout_ms = idle_timeout;
            settings_modified_ = true;
        }

        if (ImGui::Checkbox("Умная перерисовка", &perf.enable_smart_redraw)) {
            settings_modified_ = true;
        }
        HelpMarker("Перерисовывать только при изменениях (экономит CPU)");

        if (ImGui::Checkbox("Показать оверлей производительности", &perf.show_performance_overlay)) {
            settings_modified_ = true;
        }
    }
}

void SettingsDialog::render_file_settings() {
    auto& files = settings_.files();

    if (ImGui::CollapsingHeader("Пути по умолчанию", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Default save path
        char save_path[512];
        std::strncpy(save_path, files.default_save_path.c_str(), sizeof(save_path) - 1);
        save_path[sizeof(save_path) - 1] = '\0';
        if (ImGui::InputText("Путь сохранения", save_path, sizeof(save_path))) {
            files.default_save_path = save_path;
            settings_modified_ = true;
        }

        // Auto-save path
        char auto_path[512];
        std::strncpy(auto_path, files.auto_save_path.c_str(), sizeof(auto_path) - 1);
        auto_path[sizeof(auto_path) - 1] = '\0';
        if (ImGui::InputText("Путь автосохранения", auto_path, sizeof(auto_path))) {
            files.auto_save_path = auto_path;
            settings_modified_ = true;
        }
    }

    if (ImGui::CollapsingHeader("Автосохранение", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Включить автосохранение", &files.auto_save_enabled)) {
            settings_modified_ = true;
        }

        int interval = files.auto_save_interval;
        if (ImGui::SliderInt("Интервал (сек)", &interval, 30, 1800)) {
            files.auto_save_interval = interval;
            settings_modified_ = true;
        }
    }
}

void SettingsDialog::render_security_settings() {
    auto& server = settings_.server();

    if (ImGui::CollapsingHeader("SSL / TLS", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Проверять SSL-сертификаты", &server.verify_ssl)) {
            settings_modified_ = true;
        }
        HelpMarker("Отключите только если используете самоподписанные сертификаты");
    }

    if (ImGui::CollapsingHeader("Токен доступа", ImGuiTreeNodeFlags_DefaultOpen)) {
        char token[256];
        std::strncpy(token, server.auth_token.c_str(), sizeof(token) - 1);
        token[sizeof(token) - 1] = '\0';
        if (ImGui::InputText("Auth Token", token, sizeof(token), ImGuiInputTextFlags_Password)) {
            server.auth_token = token;
            settings_modified_ = true;
        }
        HelpMarker("Токен для аутентификации на сервере (если включена)");
    }
}

} // namespace ui
} // namespace llama_gui
