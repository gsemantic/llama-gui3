#include "dialog_manager.h"

namespace llama_gui {
namespace ui {

// DialogFactory implementation

namespace DialogFactory {

Dialog createHelpDialog() {
    Dialog dialog;
    dialog.type = DialogType::Help;
    dialog.title = "Help";
    dialog.message = "Llama GUI - Chat Interface for Language Models";
    dialog.visible = false;
    return dialog;
}

Dialog createAboutDialog() {
    Dialog dialog;
    dialog.type = DialogType::About;
    dialog.title = "About Llama GUI";
    dialog.message = "Version 1.0.0\nA modern chat interface for language models.";
    dialog.visible = false;
    return dialog;
}

Dialog createKeyboardShortcutsDialog() {
    Dialog dialog;
    dialog.type = DialogType::KeyboardShortcuts;
    dialog.title = "Keyboard Shortcuts";
    dialog.message = "Available keyboard shortcuts:";
    dialog.visible = false;
    return dialog;
}

Dialog createServerStatusDialog(const std::string& status) {
    Dialog dialog;
    dialog.type = DialogType::ServerStatus;
    dialog.title = "Server Status";
    dialog.message = status;
    dialog.visible = false;
    return dialog;
}

Dialog createConfirmationDialog(const std::string& title, const std::string& message,
                              std::function<void(bool)> callback) {
    Dialog dialog;
    dialog.type = DialogType::Confirmation;
    dialog.title = title;
    dialog.message = message;
    dialog.visible = false;

    // Добавляем кнопки
    dialog.buttons.push_back({"Yes", [callback]() { if (callback) callback(true); }, true, false});
    dialog.buttons.push_back({"No", [callback]() { if (callback) callback(false); }, false, true});

    return dialog;
}

Dialog createInfoDialog(const std::string& title, const std::string& message) {
    Dialog dialog;
    dialog.type = DialogType::Info;
    dialog.title = title;
    dialog.message = message;
    dialog.visible = false;
    return dialog;
}

Dialog createErrorDialog(const std::string& title, const std::string& message) {
    Dialog dialog;
    dialog.type = DialogType::Error;
    dialog.title = title;
    dialog.message = message;
    dialog.visible = false;
    return dialog;
}

Dialog createFileOpenDialog(const std::string& title, const std::string& filter) {
    Dialog dialog;
    dialog.type = DialogType::FileOpen;
    dialog.title = title.empty() ? "Open File" : title;
    dialog.message = "Select a file to open";
    dialog.visible = false;
    return dialog;
}

Dialog createFileSaveDialog(const std::string& title, const std::string& default_name) {
    Dialog dialog;
    dialog.type = DialogType::FileSave;
    dialog.title = title.empty() ? "Save File" : title;
    dialog.message = "Enter a filename to save";
    dialog.visible = false;
    return dialog;
}

} // namespace DialogFactory

} // namespace ui
} // namespace llama_gui
