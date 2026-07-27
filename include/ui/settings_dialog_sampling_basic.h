#pragma once

#include <string>
#include "core/settings.h"

namespace llama_gui {
namespace ui {

// Forward reference
using Settings = llama_gui::core::Settings;

/**
 * @brief UI компонент для базовых настроек сэмплирования
 * 
 * Вкладки:
 * - Basic (temperature, top-k, top-p, min-p)
 * - Advanced (typical, TFS)
 * - Mirostat (адаптивный контроль энтропии)
 */
class SamplingBasicDialog {
public:
    explicit SamplingBasicDialog(Settings& settings);
    ~SamplingBasicDialog();

    /// Рендеринг диалога
    void render();

    /// Проверка изменения настроек
    bool is_modified() const { return modified_; }
    void set_modified(bool value) { modified_ = value; }

private:
    // Рендеринг секций
    void render_basic_sampling_section();
    void render_advanced_sampling_section();
    void render_mirostat_section();

    // Вспомогательные методы
    void HelpMarker(const std::string& desc);

    // Ссылка на настройки
    Settings& settings_;
    bool modified_ = false;
};

} // namespace ui
} // namespace llama_gui
