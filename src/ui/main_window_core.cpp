#include "main_window.h"
#include "advanced_menu_system.h"
#include "localization_manager.h"
#include <iostream>

namespace llama_gui {
namespace ui {

void MainWindow::render_main_layout() {
    // Layout is managed by WindowManager/WindowCoordinator
    // This method is kept for compatibility but does not render components
    // to avoid double-rendering and input conflicts
}

void MainWindow::render_status_bar() {
    if (layout_controller_.isDirtyStatusBar()) {
        layout_controller_.setDirtyStatusBar(false);
    }

    // Фиксированная полоса внизу экрана
    ImVec2 display_size = ImGui::GetIO().DisplaySize;
    float bar_height = 48.0f;
    ImGui::SetNextWindowPos(ImVec2(0, display_size.y - bar_height));
    ImGui::SetNextWindowSize(ImVec2(display_size.x, bar_height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));

    ImGui::Begin("##StatusBar", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavFocus);

    // Cloud mode: показываем облачного провайдера/модель
    const auto& cloud = settings_.cloud_provider();
    if (cloud.enabled) {
        ImGui::TextColored(ImVec4(0.0f, 0.7f, 1.0f, 1.0f), TRF("status.cloud", "Cloud: %s"), cloud.model_id.c_str());
    } else {
        // Server status
        if (server_manager_) {
            bool running = server_manager_->is_server_running();
            if (running) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), TRF("status.server_running", "Server: Running"));
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), TRF("status.server_stopped", "Server: Stopped"));
            }
            ImGui::SameLine();
        }

        // Model info (короткое имя файла модели)
        std::string model_path = settings_.get_model_path();
        size_t last_slash = model_path.find_last_of("/\\");
        std::string model_name = (last_slash == std::string::npos) ? model_path : model_path.substr(last_slash + 1);
        ImGui::Text(TRF("status.model", "Model: %s"), model_name.c_str());
    }

    // Performance metrics (скорость, токены, время, контекст)
    if (chat_interface_) {
        const auto& m = chat_interface_->get_performance_metrics();
        if (m.is_measuring) {
            ImGui::SameLine();
            if (m.total_context > 0) {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.0f, 1.0f),
                    TRF("performance.generating_short", "Generating: %d tok | %.1f tok/s | %ds | Context: %d/%d"),
                    m.tokens_generated, m.tokens_per_second, static_cast<int>(m.response_time_seconds),
                    m.context_used + m.tokens_generated, m.total_context);
            } else {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.0f, 1.0f),
                    TRF("performance.generating_short_noctx", "Generating: %d tok | %.1f tok/s | %ds | Context: %d"),
                    m.tokens_generated, m.tokens_per_second, static_cast<int>(m.response_time_seconds),
                    m.context_used + m.tokens_generated);
            }
        } else if (m.response_time_seconds > 0) {
            ImGui::SameLine();
            if (m.total_context > 0) {
                ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f),
                    TRF("performance.completed_short", "✓ %d tok | %.1f tok/s | %ds | Context: %d/%d"),
                    m.tokens_generated, m.tokens_per_second, static_cast<int>(m.response_time_seconds),
                    m.context_used, m.total_context);
            } else {
                ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f),
                    TRF("performance.completed_short_noctx", "✓ %d tok | %.1f tok/s | %ds | Context: %d"),
                    m.tokens_generated, m.tokens_per_second, static_cast<int>(m.response_time_seconds),
                    m.context_used);
            }
        }
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

void MainWindow::cleanup_sdl2() {
#ifdef USE_SDL2
    SDL_Quit();
#endif
}

void MainWindow::load_fonts_with_cyrillic() {
    ImGuiIO& io = ImGui::GetIO();
    float font_size = settings_.display().font_size;

    // Get executable directory for font paths
    char exe_path[1024];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    std::string exe_dir = (len > 0) ? std::string(exe_path, len) : ".";
    exe_dir = exe_dir.substr(0, exe_dir.find_last_of('/'));

    // Try to load system fonts with Cyrillic support
    const char* cyrillic_fonts[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        nullptr
    };

    bool loaded_cyrillic = false;
    for (const char* font_path : cyrillic_fonts) {
        if (font_path && access(font_path, F_OK) == 0) {
            ImFontConfig config;
            config.OversampleH = 2;
            config.OversampleV = 2;
            config.PixelSnapH = true;
            io.Fonts->AddFontFromFileTTF(font_path, font_size, &config, io.Fonts->GetGlyphRangesCyrillic());
            loaded_cyrillic = true;
            break;
        }
    }

    // If no system font found, use ImGui default (will miss Cyrillic)
    if (!loaded_cyrillic) {
        io.Fonts->AddFontDefault();
    }

    // Try multiple font paths for FontAwesome
    std::vector<std::string> fontawesome_paths = {
        exe_dir + "/fonts/FontAwesomeSolid.otf",
        exe_dir + "/../fonts/FontAwesomeSolid.otf",
        "/usr/share/fonts/font-awesome/FontAwesomeSolid.otf",
        "/usr/share/fonts/truetype/font-awesome/FontAwesomeSolid.otf"
    };

    std::string fontawesome_path;
    for (const auto& path : fontawesome_paths) {
        if (access(path.c_str(), F_OK) == 0) {
            fontawesome_path = path;
            break;
        }
    }

    // Add FontAwesome merged into the main font, so icons render inline everywhere
    if (!fontawesome_path.empty()) {
        ImFontConfig icons_config;
        icons_config.MergeMode = true;
        icons_config.OversampleH = 2;
        icons_config.OversampleV = 2;
        icons_config.PixelSnapH = true;
        icons_config.GlyphMinAdvanceX = font_size * 0.9f;
        icons_config.GlyphMaxAdvanceX = font_size * 0.9f;
        icons_config.GlyphOffset = ImVec2(0.0f, font_size * 0.03f);
        // Диапазон иконок FontAwesome (private use area)
        static const ImWchar icon_ranges[] = { 0xf000, 0xf8ff, 0 };
        io.Fonts->AddFontFromFileTTF(fontawesome_path.c_str(), font_size, &icons_config, icon_ranges);
    }

    // Merge a CJK fallback font so that Chinese/Japanese/Korean text (e.g. news
    // titles fetched from foreign RSS feeds) renders instead of '?'. The glyph
    // ranges are limited to common simplified Chinese to keep atlas size sane.
    const char* cjk_fonts[] = {
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        nullptr
    };
    for (const char* cjk_path : cjk_fonts) {
        if (cjk_path && access(cjk_path, F_OK) == 0) {
            ImFontConfig cjk_config;
            cjk_config.MergeMode = true;
            cjk_config.OversampleH = 1;
            cjk_config.OversampleV = 1;
            // wqy-microhei / Noto CJK are font collections (TTC): pick first face.
            cjk_config.FontNo = 0;
            io.Fonts->AddFontFromFileTTF(cjk_path, font_size, &cjk_config,
                                        io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
            break;
        }
    }

    last_font_size_ = font_size;
}

bool MainWindow::checkAndRebuildFonts() {
    float current_size = settings_.display().font_size;
    if (std::abs(current_size - last_font_size_) < 0.01f) {
        return false;
    }

    std::cout << "[MainWindow] Font size changed: " << last_font_size_ << " -> " << current_size << ", rebuilding..." << std::endl;

    // Backend (OpenGL3) auto-rebuilds font texture on next NewFrame()
    reload_fonts();

    std::cout << "[MainWindow] Fonts reloaded with size: " << current_size << std::endl;
    return true;
}

} // namespace ui
} // namespace llama_gui
