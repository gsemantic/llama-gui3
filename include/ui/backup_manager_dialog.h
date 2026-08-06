#pragma once

#include <string>
#include <vector>
#include "../core/config_manager.h"

namespace llama_gui {
namespace ui {

/**
 * @class BackupManagerDialog
 * @brief Диалог управления резервными копиями настроек
 * 
 * Позволяет пользователю:
 * - Просматривать список резервных копий
 * - Создавать новую резервную копию
 * - Восстанавливать из резервной копии
 * - Удалять старые резервные копии
 */
class BackupManagerDialog {
public:
    explicit BackupManagerDialog(llama_gui::core::ConfigManager& config);
    ~BackupManagerDialog() = default;

    // Запрет копирования
    BackupManagerDialog(const BackupManagerDialog&) = delete;
    BackupManagerDialog& operator=(const BackupManagerDialog&) = delete;

    /**
     * @brief Отрисовка диалога
     */
    void render();

    /**
     * @brief Открыть/закрыть диалог
     */
    void setOpen(bool open) { is_open_ = open; }
    bool isOpen() const { return is_open_; }

private:
    llama_gui::core::ConfigManager& config_manager_;
    bool is_open_ = false;
    
    // Состояние UI
    std::string selected_backup_;
    std::vector<std::string> backup_files_;
    bool refresh_needed_ = true;
    
    // Флаги диалогов
    bool show_restore_confirm_ = false;
    bool show_delete_confirm_ = false;
    
    // Сообщение о статусе
    std::string status_message_;
    float status_timer_ = 0.0f;

    /**
     * @brief Обновить список резервных копий
     */
    void refreshBackupList();

    /**
     * @brief Отрисовка списка резервных копий
     */
    void renderBackupList();

    /**
     * @brief Отрисовка кнопок действий
     */
    void renderActionButtons();

    /**
     * @brief Отрисовка диалога подтверждения восстановления
     */
    void renderRestoreConfirmDialog();

    /**
     * @brief Отрисовка диалога подтверждения удаления
     */
    void renderDeleteConfirmDialog();

    /**
     * @brief Показать временное сообщение о статусе
     */
    void showStatusMessage(const std::string& message);

    /**
     * @brief Отрисовка сообщения о статусе
     */
    void renderStatusMessage();

    /**
     * @brief Извлечь дату из имени файла резервной копии
     */
    std::string extractDateFromFilename(const std::string& filename);
};

} // namespace ui
} // namespace llama_gui
