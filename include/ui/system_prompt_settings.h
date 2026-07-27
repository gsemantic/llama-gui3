#pragma once

#include "../include/core/settings.h"
#include <string>

namespace llama_gui {
namespace ui {

/**
 * @brief Класс для управления настройками системного промта
 */
class SystemPromptSettings {
public:
    explicit SystemPromptSettings(llama_gui::core::Settings& settings);
    ~SystemPromptSettings();

    /**
     * @brief Отрисовывает интерфейс настроек системного промта
     */
    void render();

    /**
     * @brief Применяет текущие настройки
     */
    void apply_settings();

    /**
     * @brief Сбрасывает настройки к значениям по умолчанию
     */
    void reset_to_default();

    /**
     * @brief Создает резервную копию текущих настроек
     */
    void backup_current_settings();

    /**
     * @brief Восстанавливает настройки из резервной копии
     */
    void restore_backup();

private:
    llama_gui::core::Settings& settings_;
    char system_prompt_buffer_[1024];  // Буфер для редактирования системного промта
    std::string backup_system_prompt_; // Резервная копия системного промта
};

} // namespace ui
} // namespace llama_gui