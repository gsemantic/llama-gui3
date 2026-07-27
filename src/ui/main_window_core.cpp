#include "main_window.h"
#include "advanced_menu_system.h"
#include <iostream>

namespace llama_gui {
namespace ui {

void MainWindow::render_frame() {
    // render_ui() is now in main_window.cpp
}

void MainWindow::render_main_layout() {
    // Layout is managed by WindowManager/WindowCoordinator
    // This method is kept for compatibility but does not render components
    // to avoid double-rendering and input conflicts
}

void MainWindow::render_menu_bar_once() {
    // Render menu bar only once after initialization
    static bool menu_built = false;
    if (!menu_built) {
        std::cout << "MainWindow: Building menu structure" << std::endl;
        advanced_menu_system_.buildModernMenu();
        menu_built = true;
    }
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

    // Server status
    if (server_manager_) {
        bool running = server_manager_->is_server_running();
        if (running) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Server: Running");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Server: Stopped");
        }
        ImGui::SameLine();
    }

    // Model info
    std::string model_info = "Model: " + settings_.get_model_path();
    ImGui::Text("%s", model_info.c_str());

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

    // Add FontAwesome as secondary font (not default)
    if (!fontawesome_path.empty()) {
        ImFontConfig config;
        config.OversampleH = 2;
        config.OversampleV = 2;
        config.PixelSnapH = true;
        float icon_size = font_size * 0.89f; // icons slightly smaller
        io.Fonts->AddFontFromFileTTF(fontawesome_path.c_str(), icon_size, &config, io.Fonts->GetGlyphRangesCyrillic());
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
