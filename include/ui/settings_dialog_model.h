#pragma once

#include <string>
#include <vector>
#include <functional>
#include "core/settings.h"

namespace llama_gui {
namespace ui {

// Forward reference
using Settings = llama_gui::core::Settings;

/**
 * @brief UI компонент для настроек загрузки модели
 * 
 * Вкладки:
 * - Model Files (основная модель, URL, HF repo)
 * - Draft Model (speculative decoding)
 * - LoRA Adapters (адаптеры)
 * - Multimodal (проекторы)
 */
class ModelSettingsDialog {
public:
    explicit ModelSettingsDialog(Settings& settings);
    ~ModelSettingsDialog();

    /// Рендеринг диалога
    void render();

    /// Проверка изменения настроек
    bool is_modified() const { return modified_; }
    void set_modified(bool value) { modified_ = value; }

    /// Колбэк для открытия диалога выбора файла модели
    using BrowseCallback = std::function<bool(std::string& out_path)>;
    void setBrowseCallback(BrowseCallback cb) { browse_callback_ = std::move(cb); }

private:
    // Рендеринг секций
    void render_model_files_section();
    void render_draft_model_section();
    void render_lora_adapters_section();
    void render_multimodal_section();

    // Вспомогательные методы
    void HelpMarker(const std::string& desc);
    void render_lora_adapter_editor(size_t index);

    // Ссылка на настройки
    Settings& settings_;
    bool modified_ = false;

    // UI state для LoRA
    char new_lora_path_[512] = "";
    float new_lora_scale_ = 1.0f;

    // UI state для Hugging Face
    bool show_hf_advanced_ = false;

    // Колбэк для выбора файла
    BrowseCallback browse_callback_;
};

} // namespace ui
} // namespace llama_gui
