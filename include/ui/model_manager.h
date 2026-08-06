#pragma once

#include <string>
#include <vector>

namespace llama_gui {
namespace core {
    class LlamaInterface;
    class Settings;
}

namespace ui {

// Forward declarations
using llama_gui::core::LlamaInterface;
using llama_gui::core::Settings;

/**
 * ModelManager - Управление загрузкой и выбором моделей
 */
class ModelManager {
public:
    ModelManager(LlamaInterface& llama_interface, Settings& settings);
    ~ModelManager() = default;

    // Загрузка моделей
    void loadModelFromPath(const std::string& model_path);

    // Диалоги
    void openModelSelectionDialog();
    void openModelDirectoryDialog();

    // Диалоги выбора файлов
    bool tryOpenModelFileDialog(std::string& selected_path);
    bool tryOpenModelDirectoryDialog(std::string& selected_path);

    // Утилиты
    std::string getPendingModelPath() const;
    void clearPendingModelPath();
    bool isModelSelectionDialogVisible() const;

private:
    LlamaInterface& llama_interface_;
    Settings& settings_;
    std::string pending_model_path_;
    bool show_model_selection_dialog_;

    // Внутренние методы
    void showModelSelectionInfo();
    void showModelSelectionError(const std::string& message);
};

} // namespace ui
} // namespace llama_gui
