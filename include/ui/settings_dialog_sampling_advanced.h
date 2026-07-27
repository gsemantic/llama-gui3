#pragma once

#include <string>
#include "core/settings.h"

namespace llama_gui {
namespace ui {

// Forward reference
using Settings = llama_gui::core::Settings;

/**
 * @brief UI компонент для продвинутых настроек сэмплирования
 * 
 * Вкладки:
 * - DRY Sampling (anti-repetition)
 * - XTC Sampling (exclusion-based)
 * - Dynamic Temperature
 * - Penalties (repeat, presence, frequency)
 * - Sampler Order
 */
class SamplingAdvancedDialog {
public:
    explicit SamplingAdvancedDialog(Settings& settings);
    ~SamplingAdvancedDialog();

    /// Рендеринг диалога
    void render();

    /// Проверка изменения настроек
    bool is_modified() const { return modified_; }
    void set_modified(bool value) { modified_ = value; }

private:
    // Рендеринг секций
    void render_dry_sampling_section();
    void render_xtc_sampling_section();
    void render_dynatemp_section();
    void render_penalties_section();
    void render_sampler_order_section();

    // Вспомогательные методы
    void HelpMarker(const std::string& desc);
    void render_dry_sequence_breaker_editor(size_t index);

    // Ссылка на настройки
    Settings& settings_;
    bool modified_ = false;

    // UI state для DRY sequence breakers
    char new_dry_breaker_[64] = "";
};

} // namespace ui
} // namespace llama_gui
