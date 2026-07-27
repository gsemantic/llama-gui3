#pragma once

#include <string>
#include "core/settings.h"

namespace llama_gui {
namespace ui {

// Forward reference
using Settings = llama_gui::core::Settings;

/**
 * @brief UI компонент для настроек логирования
 * 
 * Вкладки:
 * - Output (файл, формат)
 * - Display (цвета, префиксы)
 * - Verbosity (уровень детализации)
 */
class LoggingDialog {
public:
    explicit LoggingDialog(Settings& settings);
    ~LoggingDialog();

    /// Рендеринг диалога
    void render();

    /// Проверка изменения настроек
    bool is_modified() const { return modified_; }
    void set_modified(bool value) { modified_ = value; }

private:
    // Рендеринг секций
    void render_output_section();
    void render_display_section();
    void render_verbosity_section();

    // Вспомогательные методы
    void HelpMarker(const std::string& desc);

    // Ссылка на настройки
    Settings& settings_;
    bool modified_ = false;
};

} // namespace ui
} // namespace llama_gui
