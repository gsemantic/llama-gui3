#pragma once

#include <string>
#include "core/settings.h"

namespace llama_gui {
namespace ui {

// Forward reference
using Settings = llama_gui::core::Settings;

/**
 * @brief UI компонент для настроек контекста и кэша
 * 
 * Вкладки:
 * - Context Size (размер контекста)
 * - KV Cache Types (типы кэша)
 * - Cache Options (опции кэширования)
 * - Slot Management (управление слотами)
 */
class ContextDialog {
public:
    explicit ContextDialog(Settings& settings);
    ~ContextDialog();

    /// Рендеринг диалога
    void render();

    /// Проверка изменения настроек
    bool is_modified() const { return modified_; }
    void set_modified(bool value) { modified_ = value; }

private:
    // Рендеринг секций
    void render_context_size_section();
    void render_kv_cache_types_section();
    void render_cache_options_section();
    void render_slot_management_section();

    // Вспомогательные методы
    void HelpMarker(const std::string& desc);
    const char* get_cache_type_label(llama_gui::core::CacheSettings::CacheType type);

    // Ссылка на настройки
    Settings& settings_;
    bool modified_ = false;
};

} // namespace ui
} // namespace llama_gui
