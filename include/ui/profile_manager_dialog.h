#pragma once

#include <string>
#include <vector>
#include <functional>
#include "../core/config_manager.h"
#include "../core/settings_snapshot.h"
#include "file_dialog_helper.h"
#include "workspace_manager.h"

namespace llama_gui {
namespace ui {

/**
 * @class ProfileManagerDialog
 * @brief Диалог управления профилями настроек
 *
 * Позволяет пользователю:
 * - Просматривать список профилей
 * - Загружать профили
 * - Сохранять текущие настройки в профиль
 * - Создавать новые профили
 * - Удалять существующие профили
 * - Управлять видимостью меню для текущего профиля
 */
class ProfileManagerDialog {
public:
    explicit ProfileManagerDialog(llama_gui::core::ConfigManager& config);
    ~ProfileManagerDialog() = default;

    ProfileManagerDialog(const ProfileManagerDialog&) = delete;
    ProfileManagerDialog& operator=(const ProfileManagerDialog&) = delete;

    void render();

    void setOpen(bool open) { is_open_ = open; }
    bool isOpen() const { return is_open_; }

    void showCreateDialog();

    void setProfileLoadCallback(std::function<void(const std::string&)> callback);

    /**
     * @brief Привязать WorkspaceManager для редактирования видимости меню
     */
    void setWorkspaceManager(WorkspaceManager* wm) { workspace_manager_ = wm; }

private:
    llama_gui::core::ConfigManager& config_manager_;
    WorkspaceManager* workspace_manager_ = nullptr;
    bool is_open_ = false;
    std::function<void(const std::string&)> profile_load_callback_;

    // Состояние UI
    std::string selected_profile_;
    char new_profile_name_[256] = "";
    char profile_name_buffer_[256] = "";
    bool initialized_ = false;

    // Флаги диалогов
    bool show_create_dialog_ = false;
    bool show_delete_confirm_ = false;
    bool show_rename_dialog_ = false;
    bool show_export_dialog_ = false;

    // Перенос настроек (экспорт/импорт снимка)
    FileDialogHelper file_dialog_helper_;
    bool export_include_secrets_ = false;
    bool show_import_confirm_requested_ = false;
    std::string import_file_path_;
    llama_gui::core::SettingsSnapshot::Info import_info_;  // полное имя: внутри namespace ui имя core:: перехватывается вложенным namespace из chat_interface.h
    bool import_include_secrets_ = true;

    // Флаги для управления фокусом
    bool request_focus_create_dialog_ = false;
    bool request_focus_rename_dialog_ = false;

    // Сообщение о статусе
    std::string status_message_;
    float status_timer_ = 0.0f;

    // Workspace editor state
    bool show_workspace_section_ = false;

    void renderProfileList();
    void renderActionButtons();
    void renderTransferSection();
    void renderWorkspaceEditor();
    void renderCreateDialog();
    void renderDeleteConfirmDialog();
    void renderRenameDialog();
    void renderExportDialog();
    void renderImportDialog();
    void showStatusMessage(const std::string& message);
    void renderStatusMessage();
};

} // namespace ui
} // namespace llama_gui
