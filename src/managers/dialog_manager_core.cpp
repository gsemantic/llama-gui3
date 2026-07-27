#include "dialog_manager.h"
#include <algorithm>
#include <iostream>
#include <sstream>

namespace llama_gui {
namespace ui {

// DialogManager implementation

void DialogManager::showDialog(DialogType type, const std::string& title, const std::string& message, const std::string& details) {
    Dialog dialog = createDialog(type, title, message);
    dialog.details = details;
    addDialog(std::move(dialog));

    std::cout << "✓ Showing dialog: " << title << std::endl;

    if (on_dialog_shown_) {
        on_dialog_shown_(type);
    }
}

void DialogManager::showInfo(const std::string& title, const std::string& message) {
    showDialog(DialogType::Info, title, message);
}

void DialogManager::showWarning(const std::string& title, const std::string& message) {
    showDialog(DialogType::Warning, title, message);
}

void DialogManager::showError(const std::string& title, const std::string& message) {
    showDialog(DialogType::Error, title, message);
}

void DialogManager::showConfirmation(const std::string& title, const std::string& message,
                                   std::function<void(bool)> callback) {
    Dialog dialog = DialogFactory::createConfirmationDialog(title, message, callback);
    addDialog(std::move(dialog));
}

void DialogManager::showHelpDialog() {
    showDialog(DialogType::Help, "Help", "Llama GUI - Chat Interface for Language Models");
}

void DialogManager::showAboutDialog() {
    showDialog(DialogType::About, "About Llama GUI", "Version 1.0.0", "A modern chat interface for language models");
}

void DialogManager::showKeyboardShortcutsDialog() {
    showDialog(DialogType::KeyboardShortcuts, "Keyboard Shortcuts", "Available keyboard shortcuts:");
}

void DialogManager::showServerStatusDialog(const std::string& status) {
    Dialog dialog = DialogFactory::createServerStatusDialog(status);
    addDialog(std::move(dialog));
}

void DialogManager::showDocumentationDialog() {
    showDialog(DialogType::Documentation, "Documentation", "Llama GUI Documentation");
}

void DialogManager::showAboutDevsDialog() {
    showDialog(DialogType::AboutDevs, "About Developers", "Llama GUI - About Developers");
}

void DialogManager::showAuditLogDialog() {
    showDialog(DialogType::AuditLog, "Audit Log", "Audit Log");
}

void DialogManager::showConsoleDialog() {
    showDialog(DialogType::Console, "Console", "Console Output");
}

void DialogManager::showCheckUpdatesDialog() {
    showDialog(DialogType::CheckUpdates, "Check Updates", "Check for Updates");
}

void DialogManager::showMetricsDialog() {
    showDialog(DialogType::Metrics, "Metrics", "Performance Metrics");
}

void DialogManager::render() {
#ifdef USE_IMGUI
    for (auto& dialog : dialogs_) {
        if (!dialog.visible) continue;

        // Устанавливаем позицию и размер диалога
        if (dialog.position.x != 0 || dialog.position.y != 0) {
            ImGui::SetNextWindowPos(dialog.position);
        }
        if (dialog.size.x != 0 || dialog.size.y != 0) {
            ImGui::SetNextWindowSize(dialog.size);
        }

        // Определяем модальность диалога
        ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize;
        if (dialog.modal) {
            flags |= ImGuiWindowFlags_Modal;
        }

        // Рендерим диалог в зависимости от типа
        switch (dialog.type) {
            case DialogType::Help:
                renderHelpDialog(dialog);
                break;
            case DialogType::About:
                renderAboutDialog(dialog);
                break;
            case DialogType::KeyboardShortcuts:
                renderKeyboardShortcutsDialog(dialog);
                break;
            case DialogType::ServerStatus:
                renderServerStatusDialog(dialog);
                break;
            case DialogType::Confirmation:
                renderConfirmationDialog(dialog);
                break;
            case DialogType::Info:
                renderInfoDialog(dialog);
                break;
            case DialogType::Error:
                renderErrorDialog(dialog);
                break;
            case DialogType::FileOpen:
            case DialogType::FileSave:
                renderFileDialog(dialog);
                break;
            case DialogType::Documentation:
                renderDocumentationDialog(dialog);
                break;
            case DialogType::AboutDevs:
                renderAboutDevsDialog(dialog);
                break;
            case DialogType::AuditLog:
                renderAuditLogDialog(dialog);
                break;
            case DialogType::Console:
                renderConsoleDialog(dialog);
                break;
            case DialogType::CheckUpdates:
                renderCheckUpdatesDialog(dialog);
                break;
            case DialogType::Metrics:
                renderMetricsDialog(dialog);
                break;
            default:
                renderInfoDialog(dialog);
                break;
        }
    }

    // Удаляем закрытые диалоги
    dialogs_.erase(
        std::remove_if(dialogs_.begin(), dialogs_.end(),
            [](const Dialog& dialog) { return !dialog.visible; }),
        dialogs_.end()
    );
#else
    std::cout << "Warning: ImGui not available, dialog rendering skipped" << std::endl;
#endif
}

void DialogManager::closeDialog(DialogType type) {
    Dialog* dialog = findDialog(type);
    if (dialog) {
        dialog->visible = false;

        if (on_dialog_closed_) {
            on_dialog_closed_(type);
        }

        std::cout << "✓ Closed dialog: " << static_cast<int>(type) << std::endl;
    }
}

void DialogManager::closeAllDialogs() {
    for (auto& dialog : dialogs_) {
        dialog.visible = false;
    }

    std::cout << "✓ Closed all dialogs" << std::endl;
}

bool DialogManager::isDialogVisible(DialogType type) const {
    const Dialog* dialog = findDialog(type);
    return dialog && dialog->visible;
}

bool DialogManager::hasVisibleDialogs() const {
    for (const auto& dialog : dialogs_) {
        if (dialog.visible) {
            return true;
        }
    }
    return false;
}

void DialogManager::setDialogPosition(DialogType type, const ImVec2& position) {
    Dialog* dialog = findDialog(type);
    if (dialog) {
        dialog->position = position;
    }
}

void DialogManager::setDialogSize(DialogType type, const ImVec2& size) {
    Dialog* dialog = findDialog(type);
    if (dialog) {
        dialog->size = size;
    }
}

DialogManager::Statistics DialogManager::getStatistics() const {
    Statistics stats;
    stats.total_dialogs = dialogs_.size();

    for (const auto& dialog : dialogs_) {
        if (dialog.visible) {
            stats.visible_dialogs++;
        }
    }

    for (const auto& pair : show_count_) {
        stats.total_shows += pair.second;
    }

    return stats;
}

void DialogManager::setDialogShownCallback(std::function<void(DialogType)> callback) {
    on_dialog_shown_ = std::move(callback);
}

void DialogManager::setDialogClosedCallback(std::function<void(DialogType)> callback) {
    on_dialog_closed_ = std::move(callback);
}

Dialog* DialogManager::findDialog(DialogType type) {
    for (auto& dialog : dialogs_) {
        if (dialog.type == type && dialog.visible) {
            return &dialog;
        }
    }
    return nullptr;
}

const Dialog* DialogManager::findDialog(DialogType type) const {
    for (const auto& dialog : dialogs_) {
        if (dialog.type == type && dialog.visible) {
            return &dialog;
        }
    }
    return nullptr;
}

Dialog DialogManager::createDialog(DialogType type, const std::string& title, const std::string& message) {
    Dialog dialog;
    dialog.type = type;
    dialog.title = title;
    dialog.message = message;
    dialog.visible = true;
    dialog.modal = true;

    // Центрируем диалог
    dialog.position = DialogUtils::centerDialogOnScreen(dialog.size);

    // Устанавливаем стандартные кнопки
    switch (type) {
        case DialogType::Error:
        case DialogType::Warning:
        case DialogType::Info:
            dialog.buttons = DialogUtils::createStandardButtons("OK");
            break;
        case DialogType::Confirmation:
            dialog.buttons = DialogUtils::createStandardButtons("Yes", "Cancel", true);
            break;
        default:
            dialog.buttons = DialogUtils::createStandardButtons();
            break;
    }

    show_count_[type]++;
    return dialog;
}

void DialogManager::addDialog(Dialog dialog) {
    // Проверяем, есть ли уже такой диалог
    Dialog* existing = findDialog(dialog.type);
    if (existing) {
        // Обновляем существующий диалог
        *existing = dialog;
    } else {
        // Добавляем новый диалог
        dialogs_.push_back(std::move(dialog));
    }
}

void DialogManager::removeDialog(DialogType type) {
    dialogs_.erase(
        std::remove_if(dialogs_.begin(), dialogs_.end(),
            [type](const Dialog& dialog) { return dialog.type == type; }),
        dialogs_.end()
    );
}

} // namespace ui
} // namespace llama_gui
