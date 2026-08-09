#pragma once

#include <string>
#include <functional>
#include <vector>
#include <mutex>
#include "file_picker_dialog.h"

namespace llama_gui {
namespace ui {

/**
 * FileDialogManager - Управление диалогами файлов и директорий.
 *
 * pick_file / pick_directory - гарантированный путь выбора:
 *  1) всегда открывается встроенный пикер (FilePickerDialog) - ОСНОВНОЙ путь,
 *     работает без внешних программ на любом десктопе;
 *  2) если доступны нативные диалоги (zenity/kdialog/python) - они предлагаются
 *     внутри пикера как ОПЦИОНАЛЬНЫЙ ускоритель (кнопка "Системный диалог").
 * Результат нативных диалогов доставляется в главный поток через render().
 */
class FileDialogManager {
public:
    using PickerCallback = std::function<void(const std::string&)>;

    FileDialogManager();
    ~FileDialogManager() = default;

    // ========================================================================
    // Гарантированный выбор (встроенный пикер + опциональный нативный ускоритель)
    // ========================================================================

    // Выбор файла (callback вызывается с путём или "" при отмене)
    void pick_file(const std::string& title, PickerCallback callback,
                   const std::string& start_dir = "", const std::string& filter_type = "");

    // Выбор папки (callback вызывается с путём или "" при отмене)
    void pick_directory(const std::string& title, PickerCallback callback,
                        const std::string& start_dir = "");

    // Рендеринг встроенного пикера и доставка результатов нативных диалогов
    // в главный поток (вызывается каждый кадр).
    void render();

    bool is_picker_open() const;
    void cancel_picker();

    // ========================================================================
    // Синхронные диалоги (только нативные, без встроенного резерва)
    // ========================================================================

    // Открытие файлов
    bool tryOpenNativeFileDialog(std::string& selected_path, const std::string& title);
    bool tryOpenZenityFileDialog(std::string& selected_path, const std::string& title);
    bool tryOpenKdialogFileDialog(std::string& selected_path, const std::string& title);
    bool tryOpenPythonFileDialog(std::string& selected_path, const std::string& title);

    // Открытие директорий
    bool tryOpenDirectoryDialog(std::string& selected_path, const std::string& title);

    // Выбор моделей
    bool tryOpenModelFileDialog(std::string& selected_path);
    bool tryOpenModelDirectoryDialog(std::string& selected_path);

    // Утилиты
    std::string getLastOpenPath() const;
    std::string getLastSavePath() const;
    void setLastOpenPath(const std::string& path);
    void setLastSavePath(const std::string& path);

private:
    struct PendingResult {
        PickerCallback callback;
        std::string path;
    };

    void enqueue_result(PickerCallback callback, const std::string& path);

    FilePickerDialog file_picker_;
    std::vector<PendingResult> pending_results_;
    std::mutex pending_mutex_;

    std::string last_open_path_;
    std::string last_save_path_;
};

} // namespace ui
} // namespace llama_gui
