#pragma once

#include <string>
#include "core/settings.h"

namespace llama_gui {
namespace ui {

// Forward reference
using Settings = llama_gui::core::Settings;

/**
 * @brief UI компонент для настроек выполнения сервера
 * 
 * Вкладки:
 * - Network (сеть: host, port, timeout)
 * - Security (API keys, SSL)
 * - Features (embeddings, metrics, webui)
 */
class ServerRuntimeDialog {
public:
    explicit ServerRuntimeDialog(Settings& settings);
    ~ServerRuntimeDialog();

    /// Рендеринг диалога
    void render();

    /// Проверка изменения настроек
    bool is_modified() const { return modified_; }
    void set_modified(bool value) { modified_ = value; }

private:
    // Рендеринг секций
    void render_network_section();
    void render_security_section();
    void render_features_section();
    void render_kv_cache_section();

    // Вспомогательные методы
    void HelpMarker(const std::string& desc);
    void render_api_key_editor(size_t index);

    // Ссылка на настройки
    Settings& settings_;
    bool modified_ = false;

    // UI state
    char new_api_key_[256] = "";
};

} // namespace ui
} // namespace llama_gui
