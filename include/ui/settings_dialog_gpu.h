#pragma once

#include <string>
#include "core/settings.h"

namespace llama_gui {
namespace ui {

// Forward reference
using Settings = llama_gui::core::Settings;

/**
 * @brief UI компонент для настроек GPU
 * 
 * Вкладки:
 * - GPU Offload (слои, split mode)
 * - Memory (mlock, mmap, warmup)
 * - Optimization (flash attention, defrag)
 */
class GPUSettingsDialog {
public:
    explicit GPUSettingsDialog(Settings& settings);
    ~GPUSettingsDialog();

    /// Рендеринг диалога
    void render();

    /// Проверка изменения настроек
    bool is_modified() const { return modified_; }
    void set_modified(bool value) { modified_ = value; }

private:
    // Рендеринг секций
    void render_gpu_offload_section();
    void render_memory_section();
    void render_optimization_section();

    // Вспомогательные методы
    void HelpMarker(const std::string& desc);

    // Ссылка на настройки
    Settings& settings_;
    bool modified_ = false;

    // UI state для tensor split
    char tensor_split_buf_[256] = "";
};

} // namespace ui
} // namespace llama_gui
