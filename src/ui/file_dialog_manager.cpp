#include "file_dialog_manager.h"
#include "file_dialog_helper.h"
#include "file_picker_dialog.h"
#include <iostream>
#include <mutex>
#include <vector>

namespace llama_gui {
namespace ui {

FileDialogManager::FileDialogManager()
    : last_open_path_()
    , last_save_path_() {
}

void FileDialogManager::pick_file(const std::string& title, FileDialogManager::PickerCallback callback,
                                  const std::string& start_dir, const std::string& filter_type) {
    // Встроенный пикер — ОСНОВНОЙ и гарантированный путь.
    // Внешняя цепочка (zenity/kdialog/python) — только опциональный ускоритель.
    FilePickerDialog::NativeAccelerator accelerator;
    if (FileDialogHelper::is_available()) {
        accelerator = [this, title, filter_type, callback]() {
            FileDialogHelper helper;
            helper.open_file_dialog(title, [this, callback](const std::string& path) {
                enqueue_result(callback, path);
            }, filter_type);
        };
    }
    file_picker_.set_native_accelerator(std::move(accelerator));
    file_picker_.open(FilePickerDialog::Mode::File, title, start_dir, std::move(callback));
}

void FileDialogManager::pick_directory(const std::string& title, FileDialogManager::PickerCallback callback,
                                       const std::string& start_dir) {
    // Встроенный пикер — ОСНОВНОЙ и гарантированный путь.
    // Внешняя цепочка (zenity/kdialog/python) — только опциональный ускоритель.
    FilePickerDialog::NativeAccelerator accelerator;
    if (FileDialogHelper::is_available()) {
        accelerator = [this, title, callback]() {
            FileDialogHelper helper;
            helper.open_directory_dialog(title, [this, callback](const std::string& path) {
                enqueue_result(callback, path);
            });
        };
    }
    file_picker_.set_native_accelerator(std::move(accelerator));
    file_picker_.open(FilePickerDialog::Mode::Directory, title, start_dir, std::move(callback));
}

void FileDialogManager::render() {
    std::vector<FileDialogManager::PendingResult> ready;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        ready.swap(pending_results_);
    }
    for (auto& result : ready) {
        if (result.callback) {
            result.callback(result.path);
        }
    }
    file_picker_.render();
}

bool FileDialogManager::is_picker_open() const {
    return file_picker_.is_open();
}

void FileDialogManager::cancel_picker() {
    file_picker_.cancel();
}

void FileDialogManager::enqueue_result(FileDialogManager::PickerCallback callback, const std::string& path) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_results_.push_back({std::move(callback), path});
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
