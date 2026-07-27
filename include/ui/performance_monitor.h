#pragma once

#include <string>
#include <vector>
#include <atomic>

#ifdef USE_IMGUI
#include "../external/imgui/imgui.h"
#endif

namespace llama_gui {
namespace ui {

/**
 * PerformanceMonitor - Мониторинг производительности и отладочные окна
 */
class PerformanceMonitor {
public:
    PerformanceMonitor();
    ~PerformanceMonitor() = default;

    // Обновление метрик
    void updateFrameTime(float frame_time);
    void updatePerformanceMetrics();
    
    // Отладочные окна
    void renderMetricsWindow();
    void renderLoggerWindow();
    void renderCommandManagerWindow();
    void renderWindowManagerWindow();
    void renderDeveloperTools();
    
    // Публичные флаги
    bool show_metrics_window_;
    bool show_debug_log_window_;
    bool show_command_manager_window_;
    bool show_window_manager_window_;
    bool show_logger_info_window_;
    bool developer_mode_enabled_;
    
    // Доступ к метрикам
    float getFrameTime() const { return frame_time_; }
    float getFPS() const { return fps_; }
    int getFrameCount() const { return frame_count_; }
    
private:
    float frame_time_;
    float fps_;
    int frame_count_;
    uint32_t last_frame_time_;
    uint32_t last_performance_update_time_;
    bool is_idle_;
    std::atomic<int> pending_dialog_results_size_;
    
    // Внутренние методы
    void renderDebugLogWindow();
    void renderMetricsWindowImpl();
    void renderCommandManagerWindowImpl();
    void renderWindowManagerWindowImpl();
    void renderDeveloperToolsImpl();
};

} // namespace ui
} // namespace llama_gui
