#include "../include/core/settings.h"
#include <filesystem>

namespace llama_gui {
namespace core {

std::string Settings::get_model_path() const {
    // Сначала проверяем model_loading_settings_.model_path
    if (!model_loading_settings_.model_path.empty()) {
        return model_loading_settings_.model_path;
    }
    
    // Затем проверяем custom settings
    if (has_custom_setting("model_path")) {
        return get_custom_setting("model_path");
    }
    
    return "/home/Alex/projects/gguf/"; // Default path to actual models directory
}

void Settings::set_model_path(const std::string& model_path) {
    // Обновляем оба места для синхронизации
    model_loading_settings_.model_path = model_path;
    set_custom_setting("model_path", model_path);

    // Auto-generate alias from filename if it's empty or matches a previous auto-generated value
    if (!model_path.empty()) {
        std::string filename = std::filesystem::path(model_path).stem().string();
        if (!filename.empty()) {
            model_loading_settings_.model_alias = filename;
        }
    }
}

std::string Settings::get_model_alias() const {
    return model_loading_settings_.model_alias;
}

void Settings::set_model_alias(const std::string& model_alias) {
    model_loading_settings_.model_alias = model_alias;
}

std::string Settings::get_embedding_model_path() const {
    // Return embedding model path from RAG settings
    return rag_settings_.embedding_model_path;
}

void Settings::set_embedding_model_path(const std::string& model_path) {
    rag_settings_.embedding_model_path = model_path;
}

// Custom settings methods
std::string Settings::get_custom_setting(const std::string& key, const std::string& default_value) const {
    auto it = custom_settings_.find(key);
    if (it != custom_settings_.end()) {
        return it->second;
    }
    return default_value;
}

bool Settings::has_custom_setting(const std::string& key) const {
    return custom_settings_.find(key) != custom_settings_.end();
}

void Settings::set_custom_setting(const std::string& key, const std::string& value) {
    custom_settings_[key] = value;
}

void Settings::remove_custom_setting(const std::string& key) {
    custom_settings_.erase(key);
}

} // namespace core
} // namespace llama_gui
