#include "file_dialog_manager.h"
#include "file_dialog_helper.h"
#include <iostream>

namespace llama_gui {
namespace ui {

FileDialogManager::FileDialogManager()
    : last_open_path_()
    , last_save_path_() {
}

bool FileDialogManager::tryOpenNativeFileDialog(std::string& selected_path, const std::string& title) {
#ifdef USE_SDL2
    FileDialogHelper helper;
    if (helper.open_file_dialog_sync(title, selected_path)) {
        if (!selected_path.empty()) {
            last_open_path_ = selected_path;
            return true;
        }
    }
#else
    return false;
#endif

    return false;
}

bool FileDialogManager::tryOpenZenityFileDialog(std::string& selected_path, const std::string& title) {
    FileDialogHelper helper;
    if (helper.open_file_dialog_sync(title, selected_path)) {
        if (!selected_path.empty()) {
            last_open_path_ = selected_path;
            return true;
        }
    }

    return false;
}

bool FileDialogManager::tryOpenKdialogFileDialog(std::string& selected_path, const std::string& title) {
    FileDialogHelper helper;
    if (helper.open_file_dialog_sync(title, selected_path)) {
        if (!selected_path.empty()) {
            last_open_path_ = selected_path;
            return true;
        }
    }

    return false;
}

bool FileDialogManager::tryOpenPythonFileDialog(std::string& selected_path, const std::string& title) {
    FileDialogHelper helper;
    if (helper.open_file_dialog_sync(title, selected_path)) {
        if (!selected_path.empty()) {
            last_open_path_ = selected_path;
            return true;
        }
    }

    return false;
}

bool FileDialogManager::tryOpenDirectoryDialog(std::string& selected_path, const std::string& title) {
#ifdef USE_SDL2
    FileDialogHelper helper;
    if (helper.open_directory_dialog_sync(title, selected_path)) {
        if (!selected_path.empty()) {
            last_open_path_ = selected_path;
            return true;
        }
    }
#else
    return false;
#endif

    return false;
}

bool FileDialogManager::tryOpenModelFileDialog(std::string& selected_path) {
    std::string title = "Select Model File";

#ifdef USE_SDL2
    FileDialogHelper helper;
    if (helper.open_file_dialog_sync(title, selected_path, "model_files")) {
        if (!selected_path.empty()) {
            last_open_path_ = selected_path;
            return true;
        }
    }
#else
    return false;
#endif

    return false;
}

bool FileDialogManager::tryOpenModelDirectoryDialog(std::string& selected_path) {
#ifdef USE_SDL2
    FileDialogHelper helper;
    if (helper.open_directory_dialog_sync("Select Models Directory", selected_path)) {
        if (!selected_path.empty()) {
            last_open_path_ = selected_path;
            return true;
        }
    }
#else
    return false;
#endif

    return false;
}

std::string FileDialogManager::getLastOpenPath() const {
    return last_open_path_;
}

std::string FileDialogManager::getLastSavePath() const {
    return last_save_path_;
}

void FileDialogManager::setLastOpenPath(const std::string& path) {
    last_open_path_ = path;
}

void FileDialogManager::setLastSavePath(const std::string& path) {
    last_save_path_ = path;
}

} // namespace ui
} // namespace llama_gui
