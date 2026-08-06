#pragma once

#include <string>
#include <functional>
#include <memory>
#include <optional>
#include <atomic>

namespace llama_gui {
namespace ui {

/**
 * Helper class for platform-independent file dialogs
 * Supports zenity, kdialog, and Python tkinter dialogs
 * 
 * Фильтры файлов:
 * - "all_files" — все файлы
 * - "text_files" — текстовые файлы (.txt, .md, .json)
 * - "model_files" — файлы моделей (.gguf, .bin, .ggml, .pth, .pt, .ckpt)
 * - "embedding_model_files" — embedding модели (.gguf, .bin, .ggml)
 * - "json_files" — JSON файлы (.json)
 * - "document_files" — документы (.pdf, .doc, .docx, .odt)
 */
class FileDialogHelper {
public:
    using FileDialogCallback = std::function<void(const std::string&)>;

    FileDialogHelper() = default;
    ~FileDialogHelper() = default;

    // ========================================================================
    // Асинхронные диалоги (callback)
    // ========================================================================

    // Open file selection dialog (без фильтра)
    void open_file_dialog(const std::string& title, FileDialogCallback callback);

    // Open file selection dialog (с фильтром)
    void open_file_dialog(const std::string& title, FileDialogCallback callback,
                          const std::string& filter_type);

    // Open directory selection dialog
    void open_directory_dialog(const std::string& title, FileDialogCallback callback);

    // Open file save dialog (без фильтра)
    void save_file_dialog(const std::string& title, const std::string& default_name,
                          FileDialogCallback callback);

    // Open file save dialog (с фильтром)
    void save_file_dialog(const std::string& title, const std::string& default_name,
                          FileDialogCallback callback, const std::string& filter_type);

    // ========================================================================
    // Синхронные диалоги (блокирующие)
    // ========================================================================

    // Open file selection dialog (синхронно)
    bool open_file_dialog_sync(const std::string& title, std::string& selected_path,
                               const std::string& filter_type = "");

    // Open directory selection dialog (синхронно)
    bool open_directory_dialog_sync(const std::string& title, std::string& selected_path);

    // Open file save dialog (синхронно)
    bool save_file_dialog_sync(const std::string& title, const std::string& default_name,
                               std::string& selected_path, const std::string& filter_type = "");

    // ========================================================================
    // Проверка доступности
    // ========================================================================

    // Check if file dialogs are available
    static bool is_available();

    // Проверка доступности конкретного инструмента
    static bool is_zenity_available();
    static bool is_kdialog_available();
    static bool is_python_available();

private:
    // ========================================================================
    // Платформенно-специфичные реализации (с фильтрами)
    // ========================================================================
    
    bool try_zenity_file_dialog(const std::string& title, bool is_save, bool is_directory,
                                const std::string& default_name, const std::string& filter_type,
                                std::string& selected_path);
    
    bool try_kdialog_file_dialog(const std::string& title, bool is_save, bool is_directory,
                                 const std::string& default_name, const std::string& filter_type,
                                 std::string& selected_path);
    
    bool try_python_file_dialog(const std::string& title, bool is_save, bool is_directory,
                                const std::string& default_name, const std::string& filter_type,
                                std::string& selected_path);

    // Helper to get temp file path
    std::string get_temp_file_path(const std::string& prefix) const;

    // ========================================================================
    // Кэширование доступности инструментов
    // ========================================================================
    
    static std::optional<bool> zenity_available_;
    static std::optional<bool> kdialog_available_;
    static std::optional<bool> python_available_;
    
    static void check_availability();
};

} // namespace ui
} // namespace llama_gui
