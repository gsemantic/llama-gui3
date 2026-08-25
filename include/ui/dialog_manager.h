#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>

#include "../external/imgui/imgui.h"

namespace llama_gui {
namespace ui {

// Типы диалогов
enum class DialogType {
    Help,
    About,
    Settings,
    FileOpen,
    FileSave,
    KeyboardShortcuts,
    Error,
    Confirmation,
    Info,
    Warning,
    ModelSelection,
    ServerStatus,
    Performance,
    Documentation,
    AboutDevs,
    AuditLog,
    Console,
    CheckUpdates,
    Metrics
};

// Структура для описания диалога
struct Dialog {
    DialogType type;
    std::string title;
    std::string message;
    std::string details;
    bool visible = false;
    bool modal = true;
    bool popup_opened = false;  // OpenPopup() уже вызван для этого кадра-цикла
    ImVec2 position = ImVec2(0, 0);
    ImVec2 size = ImVec2(400, 300);
    
    // Кнопки диалога
    struct Button {
        std::string text;
        std::function<void()> callback;
        bool is_default = false;
        bool is_cancel = false;
    };
    
    std::vector<Button> buttons;
    
    // Данные для специфичных диалогов
    struct DialogData {
        std::string file_path;
        std::string selected_item;
        std::vector<std::string> options;
        std::unordered_map<std::string, std::string> key_value_pairs;
    } data;
};

// Менеджер диалогов
class DialogManager {
public:
    DialogManager() = default;
    ~DialogManager() = default;
    
    // Удалить копирование
    DialogManager(const DialogManager&) = delete;
    DialogManager& operator=(const DialogManager&) = delete;
    
    // Показать диалог
    void showDialog(DialogType type, const std::string& title, const std::string& message, 
                   const std::string& details = "");
    void showInfo(const std::string& title, const std::string& message);
    void showWarning(const std::string& title, const std::string& message);
    void showError(const std::string& title, const std::string& message);
    void showConfirmation(const std::string& title, const std::string& message,
                         std::function<void(bool)> callback);
    
    // Специальные диалоги
    void showHelpDialog();
    void showAboutDialog();
    void showKeyboardShortcutsDialog();
    void showServerStatusDialog(const std::string& status);
    void showDocumentationDialog();
    void showAboutDevsDialog();
    void showAuditLogDialog();
    void showConsoleDialog();
    void showCheckUpdatesDialog();
    void showMetricsDialog();
    
    // Рендеринг всех активных диалогов
    void render();
    
    // Закрытие диалогов
    void closeDialog(DialogType type);
    void closeAllDialogs();
    
    // Проверка видимости
    bool isDialogVisible(DialogType type) const;
    bool hasVisibleDialogs() const;
    
    // Настройка позиции и размера
    void setDialogPosition(DialogType type, const ImVec2& position);
    void setDialogSize(DialogType type, const ImVec2& size);
    
    // Получение статистики
    struct Statistics {
        size_t total_dialogs = 0;
        size_t visible_dialogs = 0;
        size_t total_shows = 0;
    };
    
    Statistics getStatistics() const;
    
    // Установка обработчиков событий
    void setDialogShownCallback(std::function<void(DialogType)> callback);
    void setDialogClosedCallback(std::function<void(DialogType)> callback);
    
private:
    std::vector<Dialog> dialogs_;
    
    // Найденные диалоги
    Dialog* findDialog(DialogType type);
    const Dialog* findDialog(DialogType type) const;
    
    // Создание диалогов
    Dialog createDialog(DialogType type, const std::string& title, const std::string& message);
    
    // Рендеринг специфичных диалогов
    void renderHelpDialog(Dialog& dialog);
    void renderAboutDialog(Dialog& dialog);
    void renderKeyboardShortcutsDialog(Dialog& dialog);
    void renderServerStatusDialog(Dialog& dialog);
    void renderConfirmationDialog(Dialog& dialog);
    void renderInfoDialog(Dialog& dialog);
    void renderErrorDialog(Dialog& dialog);
    void renderFileDialog(Dialog& dialog);
    void renderDocumentationDialog(Dialog& dialog);
    void renderAboutDevsDialog(Dialog& dialog);
    void renderAuditLogDialog(Dialog& dialog);
    void renderConsoleDialog(Dialog& dialog);
    void renderCheckUpdatesDialog(Dialog& dialog);
    void renderMetricsDialog(Dialog& dialog);
    
    // Вспомогательные методы
    void addDialog(Dialog dialog);
    void removeDialog(DialogType type);
    
    // Статистика
    mutable std::unordered_map<DialogType, size_t> show_count_;
    
    // Обработчики событий
    std::function<void(DialogType)> on_dialog_shown_;
    std::function<void(DialogType)> on_dialog_closed_;
};

// Фабрика для создания стандартных диалогов
namespace DialogFactory {
    
    // Создать диалог помощи
    Dialog createHelpDialog();
    
    // Создать диалог "О программе"
    Dialog createAboutDialog();
    
    // Создать диалог горячих клавиш
    Dialog createKeyboardShortcutsDialog();
    
    // Создать диалог статуса сервера
    Dialog createServerStatusDialog(const std::string& status);
    
    // Создать диалог подтверждения
    Dialog createConfirmationDialog(const std::string& title, const std::string& message,
                                  std::function<void(bool)> callback);
    
    // Создать информационный диалог
    Dialog createInfoDialog(const std::string& title, const std::string& message);
    
    // Создать диалог ошибки
    Dialog createErrorDialog(const std::string& title, const std::string& message);
    
    // Создать диалог открытия файла
    Dialog createFileOpenDialog(const std::string& title, const std::string& filter = "");
    
    // Создать диалог сохранения файла
    Dialog createFileSaveDialog(const std::string& title, const std::string& default_name = "");
}

// Утилиты для диалогов
namespace DialogUtils {
    
    // Центрировать диалог на экране
    ImVec2 centerDialogOnScreen(const ImVec2& dialog_size);
    
    // Создать стандартные кнопки
    std::vector<Dialog::Button> createStandardButtons(const std::string& ok_text = "OK",
                                                     const std::string& cancel_text = "Cancel",
                                                     bool show_cancel = false);
    
    // Показать уведомление (неблокирующий диалог)
    void showNotification(const std::string& title, const std::string& message, 
                         int duration_ms = 3000);
    
    // Получить отформатированный текст для горячих клавиш
    std::string formatKeyboardShortcut(const std::string& shortcut);
    
    // Показать диалог прогресса
    void showProgressDialog(const std::string& title, const std::string& message,
                           float progress, const std::string& status = "");
}

} // namespace ui
} // namespace llama_gui