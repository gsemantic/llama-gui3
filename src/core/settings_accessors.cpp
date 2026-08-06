#include "../../include/core/settings.h"
#include <sstream>

namespace llama_gui {
namespace core {

// Theme methods
void Settings::set_theme(ThemeType theme) {
    current_theme_ = theme;
    display_settings_.use_dark_theme = (theme == ThemeType::Dark || theme == ThemeType::Auto);
}

ThemeType Settings::get_theme() const {
    return current_theme_;
}

bool Settings::is_dark_theme() const {
    return current_theme_ == ThemeType::Dark || current_theme_ == ThemeType::Auto;
}

// Server URL methods
void Settings::set_server_url(const std::string& url) {
    // Parse URL and update server settings
    size_t colon_pos = url.find_last_of(':');
    if (colon_pos != std::string::npos) {
        server_settings_.host = url.substr(0, colon_pos);
        try {
            server_settings_.port = std::stoi(url.substr(colon_pos + 1));
        } catch (const std::exception&) {
            server_settings_.port = 8081; // Default port
        }
    } else {
        server_settings_.host = url;
        server_settings_.port = 8081; // Default port
    }
    server_settings_.api_url = "http://" + server_settings_.host + ":" + std::to_string(server_settings_.port);
}

std::string Settings::get_server_url() const {
    return server_settings_.api_url;
}

bool Settings::is_server_reachable() const {
    // Basic reachability check
    return true;
}

// Display settings methods
void Settings::set_display_settings(const DisplaySettings& settings) {
    display_settings_ = settings;
}

// Server settings methods
void Settings::set_server_settings(const ServerSettings& settings) {
    server_settings_ = settings;
}

// Chat settings methods
void Settings::set_chat_settings(const ChatSettings& settings) {
    chat_settings_ = settings;
}

// File settings methods
void Settings::set_file_settings(const FileSettings& settings) {
    file_settings_ = settings;
}

// Performance settings methods
void Settings::set_performance_settings(const PerformanceSettings& settings) {
    performance_settings_ = settings;
}

// RAG settings methods
void Settings::set_rag_settings(const RagSettings& settings) {
    rag_settings_ = settings;
}

// Debug info
std::string Settings::get_debug_info() const {
    std::ostringstream debug;
    debug << "Settings Debug Info:\n";
    debug << "Display: " << display_settings_.window_width << "x" << display_settings_.window_height << "\n";
    debug << "Screen: " << display_settings_.screen_width << "x" << display_settings_.screen_height << "\n";
    debug << "DPI Scale: " << display_settings_.dpi_scale << "\n";
    debug << "Auto Resize: " << (display_settings_.auto_resize ? "Yes" : "No") << "\n";
    debug << "Window Position: " << display_settings_.window_x << "," << display_settings_.window_y << "\n";
    debug << "Window Maximized: " << (display_settings_.window_maximized ? "Yes" : "No") << "\n";
    debug << "Server: " << server_settings_.host << ":" << server_settings_.port << "\n";
    debug << "Theme: " << static_cast<int>(current_theme_) << "\n";
    debug << "Custom settings count: " << custom_settings_.size() << "\n";
    return debug.str();
}

} // namespace core
} // namespace llama_gui
