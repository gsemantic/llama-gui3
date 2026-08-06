#include "../include/core/settings.h"
#include <iostream>
#include <sstream>
#include <cstdio>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace llama_gui {
namespace core {

using json = nlohmann::json;

Settings::Settings() {
    // Basic initialization
    std::cout << "Settings initialized" << std::endl;
    // Создаем директорию профилей если её нет
    if (!std::filesystem::exists(profiles_directory_)) {
        std::filesystem::create_directories(profiles_directory_);
    }
}

Settings::~Settings() = default;

bool Settings::reset_to_defaults() {
    std::cout << "Settings reset to defaults" << std::endl;

    // Определяем разрешение экрана
    determine_screen_resolution();

    // Адаптируем размер окна к разрешению экрана
    adapt_window_size_to_screen();

    // Reset Chat Settings
    chat_settings_ = ChatSettings(); // Use default constructor values

    return true;
}

void Settings::determine_screen_resolution() {
    // Определяем разрешение экрана и DPI
    // На Linux можно использовать xrandr или X11 функции
    #ifdef __linux__
    FILE* pipe = popen("xrandr | grep ' connected' | head -1 | grep -o '[0-9]\\+x[0-9]\\+' | head -1", "r");
    if (pipe) {
        char buffer[128];
        if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            int width, height;
            if (sscanf(buffer, "%dx%d", &width, &height) == 2) {
                display_settings_.screen_width = width;
                display_settings_.screen_height = height;
                std::cout << "Detected screen resolution: " << width << "x" << height << std::endl;
            }
        }
        pclose(pipe);
    }
    #endif

    // Определяем DPI (приблизительно)
    #ifdef __linux__
    pipe = popen("xdpyinfo | grep 'resolution:' | grep -o '[0-9]\\+x[0-9]\\+' | head -1", "r");
    if (pipe) {
        char buffer[128];
        if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            int x_res, y_res;
            if (sscanf(buffer, "%dx%d", &x_res, &y_res) == 2) {
                // Стандартный DPI 96, вычисляем масштаб
                display_settings_.dpi_scale = static_cast<float>(x_res) / 96.0f;
                std::cout << "Detected DPI scale: " << display_settings_.dpi_scale << std::endl;
            }
        }
        pclose(pipe);
    }
    #endif

    // Запасной вариант - используем стандартные значения
    if (display_settings_.screen_width <= 0 || display_settings_.screen_height <= 0) {
        display_settings_.screen_width = 1920;
        display_settings_.screen_height = 1080;
        display_settings_.dpi_scale = 1.0f;
    }
}

void Settings::adapt_window_size_to_screen() {
    if (!display_settings_.auto_resize) {
        return; // Используем сохраненные настройки
    }

    // ПОЛНОЭКРАННЫЙ РЕЖИМ: Используем всё разрешение экрана
    int target_width = display_settings_.screen_width;
    int target_height = display_settings_.screen_height;

    std::cout << "Adapted window size to fullscreen: " << target_width << "x" << target_height << std::endl;
    std::cout << "Screen: " << display_settings_.screen_width << "x" << display_settings_.screen_height << std::endl;
    std::cout << "DPI scale: " << display_settings_.dpi_scale << std::endl;
}

} // namespace core
} // namespace llama_gui
