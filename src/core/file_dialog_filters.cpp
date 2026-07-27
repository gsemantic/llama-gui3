#include "../include/core/file_dialog_filters.h"
#include <algorithm>
#include <sstream>

namespace llama_gui {
namespace core {

// Статические члены класса
std::unordered_map<std::string, FileFilter> FileDialogFilters::filters_;
bool FileDialogFilters::initialized_ = false;

// Предопределённые типы фильтров
const std::string FileDialogFilters::ALL_FILES = "all_files";
const std::string FileDialogFilters::TEXT_FILES = "text_files";
const std::string FileDialogFilters::MODEL_FILES = "model_files";
const std::string FileDialogFilters::EMBEDDING_MODEL_FILES = "embedding_model_files";
const std::string FileDialogFilters::JSON_FILES = "json_files";
const std::string FileDialogFilters::DOCUMENT_FILES = "document_files";

// ============================================================================
// Реализация FileFilter
// ============================================================================

std::string FileFilter::to_zenity_pattern() const {
    std::ostringstream oss;
    oss << name << " | ";
    
    bool first = true;
    for (const auto& ext : extensions) {
        if (!first) oss << " ";
        oss << "*" << ext;
        first = false;
    }
    
    return oss.str();
}

std::string FileFilter::to_kdialog_pattern() const {
    std::ostringstream oss;
    
    bool first = true;
    for (const auto& ext : extensions) {
        if (!first) oss << " ";
        oss << "*" << ext;
        first = false;
    }
    
    oss << "|" << name;
    return oss.str();
}

std::string FileFilter::to_python_filetypes() const {
    std::ostringstream oss;
    
    oss << "[('" << name << "', '";
    
    bool first = true;
    for (const auto& ext : extensions) {
        if (!first) oss << " ";
        oss << "*" << ext;
        first = false;
    }
    
    oss << "')]";
    return oss.str();
}

std::string FileFilter::to_extensions_string() const {
    std::ostringstream oss;
    
    bool first = true;
    for (const auto& ext : extensions) {
        if (!first) oss << " ";
        oss << "*" << ext;
        first = false;
    }
    
    return oss.str();
}

// ============================================================================
// Реализация FileDialogFilters
// ============================================================================

void FileDialogFilters::initialize() {
    if (initialized_) {
        return;
    }

    // All files
    filters_[ALL_FILES] = FileFilter{"All files", {"*"}};

    // Text files: .txt, .md, .json
    filters_[TEXT_FILES] = FileFilter{"Text files", {".txt", ".md", ".json"}};

    // Model files: .gguf, .bin, .ggml, .pth, .pt, .ckpt
    filters_[MODEL_FILES] = FileFilter{
        "Model files", {".gguf", ".bin", ".ggml", ".pth", ".pt", ".ckpt"}};

    // Embedding model files: .gguf, .bin, .ggml
    filters_[EMBEDDING_MODEL_FILES] =
        FileFilter{"Model files", {".gguf", ".bin", ".ggml"}};

    // JSON files: .json
    filters_[JSON_FILES] = FileFilter{"JSON files", {".json"}};

    // Document files: .pdf, .doc, .docx, .odt
    filters_[DOCUMENT_FILES] =
        FileFilter{"Documents", {".pdf", ".doc", ".docx", ".odt"}};

    initialized_ = true;
}

const FileFilter& FileDialogFilters::get_filter(const std::string& filter_type) {
    initialize();

    auto it = filters_.find(filter_type);
    if (it != filters_.end()) {
        return it->second;
    }

    // Возвращаем all_files по умолчанию
    return filters_[ALL_FILES];
}

bool FileDialogFilters::has_filter(const std::string& filter_type) {
    initialize();
    return filters_.find(filter_type) != filters_.end();
}

} // namespace core
} // namespace llama_gui
