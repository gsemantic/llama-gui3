#pragma once

#include <string>
#include "core/settings.h"

namespace llama_gui {
namespace ui {

// Forward reference
using Settings = llama_gui::core::Settings;

/**
 * @brief UI компонент для продвинутых настроек
 * 
 * Вкладки:
 * - Control Vectors (векторы управления)
 * - Tensor Override (переопределение тензоров)
 * - Speculative Decoding (черновое декодирование)
 */
class AdvancedDialog {
public:
    explicit AdvancedDialog(Settings& settings);
    ~AdvancedDialog();

    /// Рендеринг диалога
    void render();

    /// Проверка изменения настроек
    bool is_modified() const { return modified_; }
    void set_modified(bool value) { modified_ = value; }

private:
    // Рендеринг секций
    void render_control_vectors_section();
    void render_tensor_override_section();
    void render_speculative_decoding_section();

    // Вспомогательные методы
    void HelpMarker(const std::string& desc);
    void render_control_vector_editor(size_t index);
    void render_tensor_override_editor(size_t index);

    // Ссылка на настройки
    Settings& settings_;
    bool modified_ = false;

    // UI state для Control Vectors
    char new_cv_path_[512] = "";
    float new_cv_scale_ = 1.0f;

    // UI state для Tensor Override
    char new_to_pattern_[128] = "";
    char new_to_buffer_type_[64] = "";
};

} // namespace ui
} // namespace llama_gui
