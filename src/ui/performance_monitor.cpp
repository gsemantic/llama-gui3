#include "performance_monitor.h"
#include <SDL2/SDL.h>
#include <iostream>
#include <iomanip>

namespace llama_gui {
namespace ui {

PerformanceMonitor::PerformanceMonitor()
    : frame_time_(0.0f)
    , fps_(0.0f)
    , frame_count_(0)
    , last_frame_time_(0)
    , last_performance_update_time_(0)
    , is_idle_(false)
    , pending_dialog_results_size_(0)
    , show_metrics_window_(false)
    , show_debug_log_window_(false)
    , show_command_manager_window_(false)
    , show_window_manager_window_(false)
    , show_logger_info_window_(false)
    , developer_mode_enabled_(false) {
    
    last_frame_time_ = SDL_GetTicks();
}

void PerformanceMonitor::updateFrameTime(float frame_time) {
    frame_time_ = frame_time;
    
    uint32_t current_time = SDL_GetTicks();
    uint32_t delta_time = current_time - last_frame_time_;
    last_frame_time_ = current_time;
    
    frame_count_++;
    
    // Update FPS every second
    if (current_time - last_performance_update_time_ >= 1000) {
        fps_ = frame_count_;
        frame_count_ = 0;
        last_performance_update_time_ = current_time;
    }
}

void PerformanceMonitor::updatePerformanceMetrics() {
    // TODO: Реализовать обновление метрик производительности
}

void PerformanceMonitor::renderMetricsWindow() {
    if (!show_metrics_window_) {
        return;
    }
    
    if (ImGui::Begin("Performance Metrics", &show_metrics_window_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Frame Time: %.3f ms", frame_time_);
        ImGui::Text("FPS: %.1f", fps_);
        ImGui::Text("Frame Count: %d", frame_count_);
        
        ImGui::Separator();
        
        if (ImGui::Button("Close")) {
            show_metrics_window_ = false;
        }
    }
    ImGui::End();
}

void PerformanceMonitor::renderLoggerWindow() {
    if (!show_debug_log_window_) {
        return;
    }
    
    if (ImGui::Begin("Debug Log", &show_debug_log_window_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Debug log content would be displayed here.");
        ImGui::Separator();
        
        if (ImGui::Button("Close")) {
            show_debug_log_window_ = false;
        }
    }
    ImGui::End();
}

void PerformanceMonitor::renderCommandManagerWindow() {
    if (!show_command_manager_window_) {
        return;
    }
    
    if (ImGui::Begin("Command Manager", &show_command_manager_window_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Command manager window content.");
        ImGui::Separator();
        
        if (ImGui::Button("Close")) {
            show_command_manager_window_ = false;
        }
    }
    ImGui::End();
}

void PerformanceMonitor::renderWindowManagerWindow() {
    if (!show_window_manager_window_) {
        return;
    }
    
    if (ImGui::Begin("Window Manager", &show_window_manager_window_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Window manager window content.");
        ImGui::Separator();
        
        if (ImGui::Button("Close")) {
            show_window_manager_window_ = false;
        }
    }
    ImGui::End();
}

void PerformanceMonitor::renderDeveloperTools() {
    if (!developer_mode_enabled_) {
        return;
    }
    
    renderMetricsWindow();
    renderLoggerWindow();
    renderCommandManagerWindow();
    renderWindowManagerWindow();
}

void PerformanceMonitor::renderDebugLogWindow() {
    renderLoggerWindow();
}

void PerformanceMonitor::renderMetricsWindowImpl() {
    renderMetricsWindow();
}

void PerformanceMonitor::renderCommandManagerWindowImpl() {
    renderCommandManagerWindow();
}

void PerformanceMonitor::renderWindowManagerWindowImpl() {
    renderWindowManagerWindow();
}

void PerformanceMonitor::renderDeveloperToolsImpl() {
    renderDeveloperTools();
}

} // namespace ui
} // namespace llama_gui
