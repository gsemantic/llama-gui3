#pragma once

#include <string>
#include "core/settings.h"

namespace llama_gui {
namespace ui {

// Forward reference
using Settings = llama_gui::core::Settings;

/**
 * @brief UI компонент для настроек RoPE и масштабирования
 * 
 * Вкладки:
 * - RoPE Scaling (режим масштабирования)
 * - RoPE Parameters (частотные параметры)
 * - YaRN (параметры YaRN)
 */
class RoPEDialog {
public:
    explicit RoPEDialog(Settings& settings);
    ~RoPEDialog();

    /// Рендеринг диалога
    void render();

    /// Проверка изменения настроек
    bool is_modified() const { return modified_; }
    void set_modified(bool value) { modified_ = value; }

private:
    // Рендеринг секций
    void render_rope_scaling_section();
    void render_rope_parameters_section();
    void render_yarn_section();

    // Вспомогательные методы
    void HelpMarker(const std::string& desc);

    // Ссылка на настройки
    Settings& settings_;
    bool modified_ = false;
};

} // namespace ui
} // namespace llama_gui
