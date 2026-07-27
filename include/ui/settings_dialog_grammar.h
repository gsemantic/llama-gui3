#pragma once

#include <string>
#include "core/settings.h"

namespace llama_gui {
namespace ui {

// Forward reference
using Settings = llama_gui::core::Settings;

/**
 * @brief UI компонент для настроек грамматики и шаблонов
 * 
 * Вкладки:
 * - Grammar (грамматика)
 * - JSON Schema (JSON схема)
 * - Chat Template (шаблон чата)
 * - Reasoning (формат рассуждений)
 */
class GrammarDialog {
public:
    explicit GrammarDialog(Settings& settings);
    ~GrammarDialog();

    /// Рендеринг диалога
    void render();

    /// Проверка изменения настроек
    bool is_modified() const { return modified_; }
    void set_modified(bool value) { modified_ = value; }

private:
    // Рендеринг секций
    void render_grammar_section();
    void render_json_schema_section();
    void render_chat_template_section();
    void render_reasoning_section();

    // Вспомогательные методы
    void HelpMarker(const std::string& desc);

    // Ссылка на настройки
    Settings& settings_;
    bool modified_ = false;

    // UI state
    char temp_schema_buf_[4096] = "";
};

} // namespace ui
} // namespace llama_gui
