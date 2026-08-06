#include "model_manager.h"
#include "file_dialog_helper.h"
#include <iostream>

namespace llama_gui {
namespace ui {

using llama_gui::core::LlamaInterface;
using llama_gui::core::Settings;

ModelManager::ModelManager(LlamaInterface& llama_interface, Settings& settings)
    : llama_interface_(llama_interface)
    , settings_(settings)
    , pending_model_path_()
    , show_model_selection_dialog_(false) {
}

void ModelManager::loadModelFromPath(const std::string& model_path) {
    std::cout << "ModelManager: Loading model from path: " << model_path << std::endl;

    // TODO: Реализовать загрузку модели через llama_interface_

    std::cout << "ModelManager: Model loading started..." << std::endl;
}

void ModelManager::openModelSelectionDialog() {
    std::cout << "ModelManager: Opening model selection dialog..." << std::endl;
    show_model_selection_dialog_ = true;
    showModelSelectionInfo();
}

void ModelManager::openModelDirectoryDialog() {
    std::cout << "ModelManager: Opening model directory dialog..." << std::endl;
    show_model_selection_dialog_ = true;
}

bool ModelManager::tryOpenModelFileDialog(std::string& selected_path) {
    std::cout << "ModelManager: Opening model file dialog..." << std::endl;
    show_model_selection_dialog_ = true;
    return false;
}

bool ModelManager::tryOpenModelDirectoryDialog(std::string& selected_path) {
    std::cout << "ModelManager: Opening model directory dialog..." << std::endl;
    show_model_selection_dialog_ = true;
    return false;
}

std::string ModelManager::getPendingModelPath() const {
    return pending_model_path_;
}

void ModelManager::clearPendingModelPath() {
    pending_model_path_.clear();
}

bool ModelManager::isModelSelectionDialogVisible() const {
    return show_model_selection_dialog_;
}

void ModelManager::showModelSelectionInfo() {
    std::cout << "ModelManager: Model selection dialog opened" << std::endl;
    std::cout << "ModelManager: Please select a model file from the file dialog" << std::endl;
}

void ModelManager::showModelSelectionError(const std::string& message) {
    std::cout << "ModelManager: Error: " << message << std::endl;
}

} // namespace ui
} // namespace llama_gui
