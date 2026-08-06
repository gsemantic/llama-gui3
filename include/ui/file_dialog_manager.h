#pragma once

#include <string>

namespace llama_gui {
namespace ui {

/**
 * FileDialogManager - Управление диалогами файлов и директорий
 */
class FileDialogManager {
public:
    FileDialogManager();
    ~FileDialogManager() = default;

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
    std::string last_open_path_;
    std::string last_save_path_;
};

} // namespace ui
} // namespace llama_gui
