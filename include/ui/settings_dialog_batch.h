#pragma once

#include <string>
#include "core/settings.h"

namespace llama_gui {
namespace ui {

// Forward reference
using Settings = llama_gui::core::Settings;

/**
 * @brief UI компонент для настроек пакетной обработки
 * 
 * Вкладки:
 * - Batch Sizes (размеры пакетов)
 * - Threading (потоки)
 * - CPU Affinity (привязка к CPU)
 */
class BatchDialog {
public:
    explicit BatchDialog(Settings& settings);
    ~BatchDialog();

    /// Рендеринг диалога
    void render();

    /// Проверка изменения настроек
    bool is_modified() const { return modified_; }
    void set_modified(bool value) { modified_ = value; }

private:
    // Рендеринг секций
    void render_batch_sizes_section();
    void render_threading_section();
    void render_cpu_affinity_section();

    // Вспомогательные методы
    void HelpMarker(const std::string& desc);

    // Ссылка на настройки
    Settings& settings_;
    bool modified_ = false;
};

} // namespace ui
} // namespace llama_gui
