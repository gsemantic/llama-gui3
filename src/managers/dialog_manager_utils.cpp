#include "dialog_manager.h"
#include <algorithm>
#include <iostream>

namespace llama_gui {
namespace ui {

// DialogUtils implementation

namespace DialogUtils {

ImVec2 centerDialogOnScreen(const ImVec2& dialog_size) {
    // Простая центризация (в реальном проекте нужно учитывать размер экрана)
    return ImVec2(
        (1200.0f - dialog_size.x) * 0.5f,  // Предполагаем ширину экрана 1200
        (800.0f - dialog_size.y) * 0.5f    // Предполагаем высоту экрана 800
    );
}

std::vector<Dialog::Button> createStandardButtons(const std::string& ok_text,
                                                 const std::string& cancel_text,
                                                 bool show_cancel) {
    std::vector<Dialog::Button> buttons;

    buttons.push_back({ok_text, nullptr, true, !show_cancel});

    if (show_cancel) {
        buttons.push_back({cancel_text, nullptr, false, true});
    }

    return buttons;
}

void showNotification(const std::string& title, const std::string& message, int duration_ms) {
    std::cout << "NOTIFICATION [" << title << "]: " << message << std::endl;
    // В реальной реализации здесь был бы таймер и автозакрытие
    (void)duration_ms;
}

std::string formatKeyboardShortcut(const std::string& shortcut) {
    // Простое форматирование горячих клавиш
    std::string formatted = shortcut;
    std::replace(formatted.begin(), formatted.end(), '+', '+');
    return formatted;
}

void showProgressDialog(const std::string& title, const std::string& message,
                       float progress, const std::string& status) {
    std::cout << "PROGRESS [" << title << "]: " << message << " - "
              << (progress * 100.0f) << "%" << std::endl;
    if (!status.empty()) {
        std::cout << "Status: " << status << std::endl;
    }
    // В реальной реализации здесь был бы диалог с прогресс-баром
}

} // namespace DialogUtils

} // namespace ui
} // namespace llama_gui
