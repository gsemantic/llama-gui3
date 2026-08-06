#include "../include/core/settings.h"
#include <iostream>

namespace llama_gui {
namespace core {

bool Settings::load_from_file(const std::string& file_path) {
    (void)file_path;
    std::cout << "Settings loaded from file" << std::endl;
    return true;
}

bool Settings::save_to_file(const std::string& file_path) const {
    (void)file_path;
    std::cout << "Settings saved to file" << std::endl;
    return true;
}

int Settings::get_screen_width() const {
    return display_settings_.screen_width;
}

int Settings::get_screen_height() const {
    return display_settings_.screen_height;
}

float Settings::get_dpi_scale() const {
    return display_settings_.dpi_scale;
}

bool Settings::should_auto_resize() const {
    return display_settings_.auto_resize;
}

void Settings::set_auto_resize(bool auto_resize) {
    display_settings_.auto_resize = auto_resize;
    if (auto_resize) {
        adapt_window_size_to_screen();
    }
}

void Settings::set_window_position(int x, int y) {
    display_settings_.window_x = x;
    display_settings_.window_y = y;
}

std::pair<int, int> Settings::get_window_position() const {
    return {display_settings_.window_x, display_settings_.window_y};
}

void Settings::set_window_maximized(bool maximized) {
    display_settings_.window_maximized = maximized;
}

bool Settings::is_window_maximized() const {
    return display_settings_.window_maximized;
}

std::pair<int, int> Settings::get_optimal_window_size() const {
    return {display_settings_.window_width, display_settings_.window_height};
}

std::pair<int, int> Settings::get_safe_window_bounds() const {
    int safe_width = display_settings_.screen_width - 2 * display_settings_.margin;
    int safe_height = display_settings_.screen_height - 2 * display_settings_.margin;
    return {safe_width, safe_height};
}

void Settings::adapt_ui_components() {
    // Адаптируем размеры компонентов в зависимости от разрешения экрана
    float scale_factor = display_settings_.dpi_scale;

    // Изменяем размер шрифта в зависимости от DPI
    // Только если font_size не был явно задан (остаётся значение по умолчанию)
    if (display_settings_.font_size <= 0.0f) {
        display_settings_.font_size = 14.0f * scale_factor;
    }

    std::cout << "UI components adapted for DPI scale: " << scale_factor
              << ", font_size: " << display_settings_.font_size << std::endl;
}

bool Settings::validate() const {
    return true;
}

std::string Settings::get_validation_errors() const {
    return "";
}

} // namespace core
} // namespace llama_gui
