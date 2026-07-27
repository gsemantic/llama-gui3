#include "../include/ui/file_dialog_helper.h"
#include "../include/core/file_dialog_filters.h"
#include <iostream>
#include <fstream>
#include <thread>
#include <unistd.h>
#include <cstdlib>
#include <chrono>

namespace llama_gui {
namespace ui {

// ============================================================================
// Статические члены для кэширования доступности
// ============================================================================

std::optional<bool> FileDialogHelper::zenity_available_;
std::optional<bool> FileDialogHelper::kdialog_available_;
std::optional<bool> FileDialogHelper::python_available_;

// ============================================================================
// Проверка доступности инструментов (с кэшированием)
// ============================================================================

void FileDialogHelper::check_availability() {
    // Проверяем, если ещё не кэшировано
    if (!zenity_available_.has_value()) {
        zenity_available_ = (system("which zenity > /dev/null 2>&1") == 0);
    }
    if (!kdialog_available_.has_value()) {
        kdialog_available_ = (system("which kdialog > /dev/null 2>&1") == 0);
    }
    if (!python_available_.has_value()) {
        python_available_ = (system("which python3 > /dev/null 2>&1") == 0);
    }
}

bool FileDialogHelper::is_zenity_available() {
    check_availability();
    return zenity_available_.value_or(false);
}

bool FileDialogHelper::is_kdialog_available() {
    check_availability();
    return kdialog_available_.value_or(false);
}

bool FileDialogHelper::is_python_available() {
    check_availability();
    return python_available_.value_or(false);
}

bool FileDialogHelper::is_available() {
    check_availability();
    return zenity_available_.value_or(false) ||
           kdialog_available_.value_or(false) ||
           python_available_.value_or(false);
}

// ============================================================================
// Асинхронные диалоги
// ============================================================================

void FileDialogHelper::open_file_dialog(const std::string& title, FileDialogCallback callback) {
    open_file_dialog(title, callback, "");
}

void FileDialogHelper::open_file_dialog(const std::string& title, FileDialogCallback callback,
                                        const std::string& filter_type) {
    std::thread worker_thread([this, title, filter_type, callback]() {
        std::string selected_path;

        if (try_zenity_file_dialog(title, false, false, "", filter_type, selected_path) ||
            try_kdialog_file_dialog(title, false, false, "", filter_type, selected_path) ||
            try_python_file_dialog(title, false, false, "", filter_type, selected_path)) {
            if (callback) {
                callback(selected_path);
            }
        } else {
            if (callback) {
                callback(""); // Empty string indicates cancellation
            }
        }
    });
    worker_thread.detach();
}

void FileDialogHelper::open_directory_dialog(const std::string& title, FileDialogCallback callback) {
    std::thread worker_thread([this, title, callback]() {
        std::string selected_path;

        if (try_zenity_file_dialog(title, false, true, "", "", selected_path) ||
            try_kdialog_file_dialog(title, false, true, "", "", selected_path) ||
            try_python_file_dialog(title, false, true, "", "", selected_path)) {
            if (callback && !selected_path.empty()) {
                callback(selected_path);
            }
        } else {
            if (callback) {
                callback("");
            }
        }
    });
    worker_thread.detach();
}

void FileDialogHelper::save_file_dialog(const std::string& title, const std::string& default_name,
                                        FileDialogCallback callback) {
    save_file_dialog(title, default_name, callback, "");
}

void FileDialogHelper::save_file_dialog(const std::string& title, const std::string& default_name,
                                        FileDialogCallback callback, const std::string& filter_type) {
    std::thread worker_thread([this, title, default_name, filter_type, callback]() {
        std::string selected_path;

        if (try_zenity_file_dialog(title, true, false, default_name, filter_type, selected_path) ||
            try_kdialog_file_dialog(title, true, false, default_name, filter_type, selected_path) ||
            try_python_file_dialog(title, true, false, default_name, filter_type, selected_path)) {
            if (callback && !selected_path.empty()) {
                callback(selected_path);
            }
        } else {
            if (callback) {
                callback("");
            }
        }
    });
    worker_thread.detach();
}

// ============================================================================
// Синхронные диалоги
// ============================================================================

bool FileDialogHelper::open_file_dialog_sync(const std::string& title, std::string& selected_path,
                                             const std::string& filter_type) {
    return try_zenity_file_dialog(title, false, false, "", filter_type, selected_path) ||
           try_kdialog_file_dialog(title, false, false, "", filter_type, selected_path) ||
           try_python_file_dialog(title, false, false, "", filter_type, selected_path);
}

bool FileDialogHelper::open_directory_dialog_sync(const std::string& title, std::string& selected_path) {
    return try_zenity_file_dialog(title, false, true, "", "", selected_path) ||
           try_kdialog_file_dialog(title, false, true, "", "", selected_path) ||
           try_python_file_dialog(title, false, true, "", "", selected_path);
}

bool FileDialogHelper::save_file_dialog_sync(const std::string& title, const std::string& default_name,
                                             std::string& selected_path, const std::string& filter_type) {
    return try_zenity_file_dialog(title, true, false, default_name, filter_type, selected_path) ||
           try_kdialog_file_dialog(title, true, false, default_name, filter_type, selected_path) ||
           try_python_file_dialog(title, true, false, default_name, filter_type, selected_path);
}

// ============================================================================
// Zenity реализация
// ============================================================================

bool FileDialogHelper::try_zenity_file_dialog(const std::string& title, bool is_save, bool is_directory,
                                               const std::string& default_name, const std::string& filter_type,
                                               std::string& selected_path) {
    if (!is_zenity_available()) {
        return false;
    }

    std::cout << "FileDialogHelper: Trying zenity dialog..." << std::endl;

    std::string temp_file = get_temp_file_path("zenity");
    std::string command = "zenity";

    if (is_directory) {
        command += " --file-selection --directory";
    } else if (is_save) {
        command += " --file-selection --save --confirm-overwrite";
    } else {
        command += " --file-selection";
    }

    command += " --title=\"" + title + "\"";

    if (!default_name.empty() && is_save) {
        command += " --filename=\"" + default_name + "\"";
    }

    // Добавляем фильтры, если указан тип фильтра
    if (!is_directory && !filter_type.empty()) {
        const auto& filter = core::FileDialogFilters::get_filter(filter_type);
        command += " --file-filter=\"" + filter.to_zenity_pattern() + "\"";
        // Добавляем "All files" как запасной вариант
        command += " --file-filter=\"All files | *\"";
    }

    command += " > " + temp_file + " 2>/dev/null";

    int result = system(command.c_str());

    if (result == 0) {
        std::ifstream input_file(temp_file);
        if (input_file.is_open()) {
            std::getline(input_file, selected_path);
            input_file.close();

            if (!selected_path.empty() && selected_path.back() == '\n') {
                selected_path.pop_back();
            }

            if (!selected_path.empty()) {
                std::cout << "FileDialogHelper: Selected path: " << selected_path << std::endl;
                std::remove(temp_file.c_str());
                return true;
            }
        }
    }

    std::remove(temp_file.c_str());
    return false;
}

// ============================================================================
// KDialog реализация
// ============================================================================

bool FileDialogHelper::try_kdialog_file_dialog(const std::string& title, bool is_save, bool is_directory,
                                                const std::string& default_name, const std::string& filter_type,
                                                std::string& selected_path) {
    if (!is_kdialog_available()) {
        return false;
    }

    std::cout << "FileDialogHelper: Trying kdialog dialog..." << std::endl;

    std::string temp_file = get_temp_file_path("kdialog");
    std::string command = "kdialog";

    if (is_directory) {
        command += " --getexistingdirectory \"/\" --title=\"" + title + "\"";
    } else if (is_save) {
        command += " --getsavefilename \"/\" --title=\"" + title + "\"";
        if (!default_name.empty()) {
            command += " \"" + default_name + "\"";
        }
    } else {
        command += " --getopenfilename \"/\" --title=\"" + title + "\"";
    }

    // Добавляем фильтр, если указан
    if (!is_directory && !filter_type.empty()) {
        const auto& filter = core::FileDialogFilters::get_filter(filter_type);
        // Для kdialog добавляем фильтр перед редиректом
        // Формат: kdialog --getopenfilename <path> <filter> --title <title>
        // Перестраиваем команду
        if (is_save) {
            command = "kdialog --getsavefilename \"/\" \"" + filter.to_kdialog_pattern() + "\" --title=\"" + title + "\"";
            if (!default_name.empty()) {
                command += " \"" + default_name + "\"";
            }
        } else {
            command = "kdialog --getopenfilename \"/\" \"" + filter.to_kdialog_pattern() + "\" --title=\"" + title + "\"";
        }
    }

    command += " > " + temp_file + " 2>/dev/null";

    int result = system(command.c_str());

    if (result == 0) {
        std::ifstream input_file(temp_file);
        if (input_file.is_open()) {
            std::getline(input_file, selected_path);
            input_file.close();

            if (!selected_path.empty() && selected_path.back() == '\n') {
                selected_path.pop_back();
            }

            if (!selected_path.empty()) {
                std::cout << "FileDialogHelper: Selected path: " << selected_path << std::endl;
                std::remove(temp_file.c_str());
                return true;
            }
        }
    }

    std::remove(temp_file.c_str());
    return false;
}

// ============================================================================
// Python tkinter реализация
// ============================================================================

bool FileDialogHelper::try_python_file_dialog(const std::string& title, bool is_save, bool is_directory,
                                               const std::string& default_name, const std::string& filter_type,
                                               std::string& selected_path) {
    if (!is_python_available()) {
        return false;
    }

    std::cout << "FileDialogHelper: Trying Python dialog..." << std::endl;

    std::string temp_file = get_temp_file_path("python");
    std::string command = "python3 -c \"";

    command += "import tkinter as tk; from tkinter import filedialog; ";
    command += "root = tk.Tk(); root.withdraw(); ";

    if (is_directory) {
        command += "path = filedialog.askdirectory(title='" + title + "'); ";
    } else if (is_save) {
        std::string default_arg = default_name.empty() ? "" : ", initialfile='" + default_name + "'";
        command += "path = filedialog.asksaveasfilename(title='" + title + "'" + default_arg + "); ";
    } else {
        // Используем фильтр, если указан
        if (!filter_type.empty()) {
            const auto& filter = core::FileDialogFilters::get_filter(filter_type);
            command += "filetypes = " + filter.to_python_filetypes() + " + [('All files', '*')]; ";
        } else {
            command += "filetypes = [('All files', '*')]; ";
        }
        command += "path = filedialog.askopenfilename(title='" + title + "', filetypes=filetypes); ";
    }

    command += "if path: print(path)\" > " + temp_file + " 2>/dev/null";

    int result = system(command.c_str());

    if (result == 0) {
        std::ifstream input_file(temp_file);
        if (input_file.is_open()) {
            std::getline(input_file, selected_path);
            input_file.close();

            if (!selected_path.empty() && selected_path.back() == '\n') {
                selected_path.pop_back();
            }

            if (!selected_path.empty()) {
                std::cout << "FileDialogHelper: Selected path: " << selected_path << std::endl;
                std::remove(temp_file.c_str());
                return true;
            }
        }
    }

    std::remove(temp_file.c_str());
    return false;
}

// ============================================================================
// Утилиты
// ============================================================================

std::string FileDialogHelper::get_temp_file_path(const std::string& prefix) const {
    return "/tmp/llama_gui_" + prefix + "_output_" + std::to_string(getpid()) + ".txt";
}

} // namespace ui
} // namespace llama_gui
