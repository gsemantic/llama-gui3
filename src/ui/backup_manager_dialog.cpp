#include "../include/ui/backup_manager_dialog.h"
#include "../external/imgui/imgui.h"
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <ctime>
#include <iomanip>

namespace llama_gui {
namespace ui {

BackupManagerDialog::BackupManagerDialog(core::ConfigManager& config)
    : config_manager_(config) {
}

void BackupManagerDialog::render() {
    if (!is_open_) return;

    // Вызываем OpenPopup ДО Begin для модальных диалогов
    if (show_restore_confirm_) {
        ImGui::OpenPopup("Подтверждение восстановления");
    }
    if (show_delete_confirm_) {
        ImGui::OpenPopup("Подтверждение удаления");
    }

    if (!ImGui::Begin("Резервные копии", &is_open_)) {
        ImGui::End();
        return;
    }

    // === Информация ===
    ImGui::Text("Резервные копии настроек:");
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                       "Расположение: %s", config_manager_.getProfilesDirectory().c_str());
    ImGui::Separator();

    // === Список резервных копий ===
    renderBackupList();

    ImGui::Separator();

    // === Кнопки действий ===
    renderActionButtons();

    // === Статус сообщение ===
    renderStatusMessage();

    // === Диалоги ===
    renderRestoreConfirmDialog();
    renderDeleteConfirmDialog();

    ImGui::End();
}

void BackupManagerDialog::refreshBackupList() {
    backup_files_.clear();
    
    std::string backup_dir = "backups/configs";
    if (!std::filesystem::exists(backup_dir)) {
        std::filesystem::create_directories(backup_dir);
        return;
    }
    
    for (const auto& entry : std::filesystem::directory_iterator(backup_dir)) {
        if (entry.path().extension() == ".ini") {
            backup_files_.push_back(entry.path().filename().string());
        }
    }
    
    // Сортировка по имени (новые сверху)
    std::sort(backup_files_.begin(), backup_files_.end(), std::greater<std::string>());
    
    refresh_needed_ = false;
}

void BackupManagerDialog::renderBackupList() {
    if (refresh_needed_) {
        refreshBackupList();
    }
    
    ImGui::BeginChild("BackupList", ImVec2(0, 250), true);
    
    if (backup_files_.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Нет резервных копий");
    } else {
        for (size_t i = 0; i < backup_files_.size(); ++i) {
            const auto& file = backup_files_[i];
            bool is_selected = (file == selected_backup_);
            
            // Форматируем дату для отображения
            std::string date = extractDateFromFilename(file);
            std::string display = file;
            if (i == 0) {
                display += " (последняя)";
            }
            
            if (ImGui::Selectable(display.c_str(), is_selected)) {
                selected_backup_ = file;
            }
            
            // Контекстное меню
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Восстановить")) {
                    selected_backup_ = file;
                    show_restore_confirm_ = true;
                }
                if (ImGui::MenuItem("Удалить")) {
                    selected_backup_ = file;
                    show_delete_confirm_ = true;
                }
                ImGui::EndPopup();
            }
        }
    }
    
    ImGui::EndChild();
}

void BackupManagerDialog::renderActionButtons() {
    ImGui::Text("Действия:");
    
    float button_width = (ImGui::GetContentRegionAvail().x - 10) / 3;
    
    // Кнопка создания
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
    if (ImGui::Button("Создать копию", ImVec2(button_width, 0))) {
        std::string backup = config_manager_.createBackup();
        if (!backup.empty()) {
            showStatusMessage("Резервная копия создана: " + backup);
            refresh_needed_ = true;
        } else {
            showStatusMessage("Ошибка создания резервной копии");
        }
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    
    // Кнопка восстановления
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.8f, 1.0f));
    bool can_restore = !selected_backup_.empty();
    ImGui::BeginDisabled(!can_restore);
    if (ImGui::Button("Восстановить", ImVec2(button_width, 0))) {
        show_restore_confirm_ = true;
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor();
    ImGui::SameLine();
    
    // Кнопка удаления
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
    ImGui::BeginDisabled(!can_restore);
    if (ImGui::Button("Удалить", ImVec2(button_width, 0))) {
        show_delete_confirm_ = true;
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor();
    
    // Кнопка обновления
    ImGui::Separator();
    if (ImGui::Button("Обновить список", ImVec2(-1, 0))) {
        refresh_needed_ = true;
    }
}

void BackupManagerDialog::renderRestoreConfirmDialog() {
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Подтверждение восстановления", &show_restore_confirm_,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Восстановить настройки из резервной копии\n\"%s\"?", selected_backup_.c_str());
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
                           "Текущие настройки будут заменены!");
        ImGui::Separator();

        if (ImGui::Button("Восстановить", ImVec2(100, 0))) {
            std::string backup_path = "backups/configs/" + selected_backup_;
            if (config_manager_.restoreFromBackup(backup_path)) {
                showStatusMessage("Настройки восстановлены");
                show_restore_confirm_ = false;
            } else {
                showStatusMessage("Ошибка восстановления");
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Отмена", ImVec2(100, 0))) {
            show_restore_confirm_ = false;
        }

        ImGui::EndPopup();
    }
}

void BackupManagerDialog::renderDeleteConfirmDialog() {
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Подтверждение удаления", &show_delete_confirm_,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Удалить резервную копию\n\"%s\"?", selected_backup_.c_str());
        ImGui::Separator();

        if (ImGui::Button("Удалить", ImVec2(100, 0))) {
            std::string backup_path = "backups/configs/" + selected_backup_;
            try {
                if (std::filesystem::remove(backup_path)) {
                    showStatusMessage("Резервная копия удалена");
                    selected_backup_.clear();
                    refresh_needed_ = true;
                } else {
                    showStatusMessage("Ошибка удаления");
                }
            } catch (const std::exception& e) {
                showStatusMessage("Ошибка: " + std::string(e.what()));
            }
            show_delete_confirm_ = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Отмена", ImVec2(100, 0))) {
            show_delete_confirm_ = false;
        }
        
        ImGui::EndPopup();
    }
}

void BackupManagerDialog::showStatusMessage(const std::string& message) {
    status_message_ = message;
    status_timer_ = 5.0f;
    std::cout << "[BackupManager] " << message << std::endl;
}

void BackupManagerDialog::renderStatusMessage() {
    if (status_timer_ > 0.0f && !status_message_.empty()) {
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.0f, 1.0f));
        ImGui::Text("%s", status_message_.c_str());
        ImGui::PopStyleColor();
        
        status_timer_ -= ImGui::GetIO().DeltaTime;
        if (status_timer_ <= 0.0f) {
            status_message_.clear();
        }
    }
}

std::string BackupManagerDialog::extractDateFromFilename(const std::string& filename) {
    // Ожидаемый формат: settings_backup_20260323_143022.ini
    size_t pos = filename.find("settings_backup_");
    if (pos != std::string::npos && filename.length() > pos + 22) {
        std::string datetime = filename.substr(pos + 16, 15);
        // Форматируем: 20260323_143022 -> 2026-03-23 14:30:22
        if (datetime.length() == 15) {
            return datetime.substr(0, 4) + "-" + 
                   datetime.substr(4, 2) + "-" + 
                   datetime.substr(6, 2) + " " + 
                   datetime.substr(9, 2) + ":" + 
                   datetime.substr(11, 2) + ":" + 
                   datetime.substr(13, 2);
        }
    }
    return filename;
}

} // namespace ui
} // namespace llama_gui
