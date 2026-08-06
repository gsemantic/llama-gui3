#include "../include/ui/localization_manager.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <vector>
#include <sys/stat.h>
#include <dirent.h>
#include <cstring>
#include <nlohmann/json.hpp>

namespace llama_gui {
namespace ui {

// Global localization manager instance
static std::unique_ptr<LocalizationManager> g_localization_manager;

// Get global localization manager instance
LocalizationManager& getLocalizationManager() {
    if (!g_localization_manager) {
        g_localization_manager = std::make_unique<LocalizationManager>();
    }
    return *g_localization_manager;
}

LocalizationManager::LocalizationManager() {
    // Initialize string cache
    string_cache_.resize(STRING_CACHE_SIZE);
    
    initializeDefaultTranslations();

    // Загружаем переводы из JSON файлов
    // Пробуем несколько возможных путей к i18n директории
    std::vector<std::string> possible_paths = {
        "i18n",
        "../i18n",
        "../../i18n",
        "../../../i18n"
    };

    bool loaded = false;
    for (const auto& path : possible_paths) {
        if (loadTranslationsFromDirectory(path)) {
            loaded = true;
            break;
        }
    }

    if (!loaded) {
        std::cout << "⚠ Warning: Could not load i18n translation files" << std::endl;
    }
}

void LocalizationManager::initializeDefaultTranslations() {
    // Инициализация переводов через модульные методы
    initializeMenuTranslations();
    initializeSettingsTranslations();
    initializeCommonTranslations();

    std::cout << "✓ Localization manager initialized with " << translations_.size() << " translations" << std::endl;
}

void LocalizationManager::setCurrentLanguage(Language language) {
    if (current_language_ != language) {
        Language old_language = current_language_;
        current_language_ = language;

        std::cout << "✓ Language changed from " << getLanguageDisplayName(old_language)
                  << " to " << getLanguageDisplayName(language) << std::endl;

        // Clear translation cache to avoid stale strings
        clearTranslationCache();

        // Notify about language change
        if (language_change_callback_) {
            language_change_callback_(language, old_language);
        }
    }
}

std::string LocalizationManager::getCurrentLanguageName() const {
    return getLanguageDisplayName(current_language_);
}

std::vector<std::string> LocalizationManager::getAvailableLanguageNames() const {
    return {
        getLanguageDisplayName(Language::Russian),
        getLanguageDisplayName(Language::English)
    };
}

std::vector<Language> LocalizationManager::getAvailableLanguages() const {
    return { Language::Russian, Language::English };
}

std::string LocalizationManager::translate(const std::string& key) const {
    auto it = translations_.find(key);
    if (it != translations_.end()) {
        const Translation& translation = it->second;
        
        switch (current_language_) {
            case Language::Russian:
                return translation.russian_text.empty() ? key : translation.russian_text;
            case Language::English:
                return translation.english_text.empty() ? key : translation.english_text;
            default:
                return key;
        }
    }
    
    // Return key if translation not found (useful for development)
    return key;
}

std::string LocalizationManager::translate(const std::string& key, const std::string& fallback) const {
    auto it = translations_.find(key);
    if (it != translations_.end()) {
        const Translation& translation = it->second;
        
        switch (current_language_) {
            case Language::Russian:
                return translation.russian_text.empty() ? fallback : translation.russian_text;
            case Language::English:
                return translation.english_text.empty() ? fallback : translation.english_text;
            default:
                return fallback;
        }
    }
    
    return fallback;
}

bool LocalizationManager::hasTranslation(const std::string& key) const {
    return translations_.find(key) != translations_.end();
}

void LocalizationManager::addTranslation(const std::string& key, const std::string& russian_text, const std::string& english_text) {
    Translation translation;
    translation.key = key;
    translation.russian_text = russian_text;
    translation.english_text = english_text;
    
    translations_[key] = translation;
}

void LocalizationManager::updateTranslation(Language language, const std::string& key, const std::string& text) {
    auto it = translations_.find(key);
    if (it != translations_.end()) {
        switch (language) {
            case Language::Russian:
                it->second.russian_text = text;
                break;
            case Language::English:
                it->second.english_text = text;
                break;
        }
    }
}

bool LocalizationManager::loadTranslationsFromFile(const std::string& file_path) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        std::cerr << "❌ Failed to open translation file: " << file_path << std::endl;
        return false;
    }
    
    try {
        nlohmann::json json_data;
        file >> json_data;
        
        if (json_data.contains("translations") && json_data["translations"].is_array()) {
            for (const auto& item : json_data["translations"]) {
                if (item.contains("key") && item.contains("russian") && item.contains("english")) {
                    addTranslation(
                        item["key"].get<std::string>(),
                        item["russian"].get<std::string>(),
                        item["english"].get<std::string>()
                    );
                }
            }
            std::cout << "✓ Loaded " << json_data["translations"].size() << " translations from " << file_path << std::endl;
            return true;
        }
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "❌ JSON parsing error in " << file_path << ": " << e.what() << std::endl;
    }
    
    return false;
}

bool LocalizationManager::saveTranslationsToFile(const std::string& file_path) const {
    std::ofstream file(file_path);
    if (!file.is_open()) {
        std::cerr << "❌ Failed to create translation file: " << file_path << std::endl;
        return false;
    }
    
    try {
        nlohmann::json json_data;
        json_data["language"] = "multilingual";
        json_data["version"] = "1.0";
        
        nlohmann::json translations_array = nlohmann::json::array();
        
        for (const auto& pair : translations_) {
            nlohmann::json item;
            item["key"] = pair.first;
            item["russian"] = pair.second.russian_text;
            item["english"] = pair.second.english_text;
            translations_array.push_back(item);
        }
        
        json_data["translations"] = translations_array;
        
        file << json_data.dump(2) << std::endl;
        std::cout << "✓ Saved " << translations_.size() << " translations to " << file_path << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "❌ Error saving translations to " << file_path << ": " << e.what() << std::endl;
    }
    
    return false;
}

bool LocalizationManager::loadTranslationsFromDirectory(const std::string& directory_path) {
    try {
        // Check if directory exists using stat
        struct stat st;
        if (stat(directory_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            std::cout << "⚠ Translation directory does not exist: " << directory_path << std::endl;
            return false;
        }
        
        bool success = true;
        for (const auto& language : getAvailableLanguages()) {
            std::string filename = getLanguageFileName(language);
            std::string file_path = directory_path + "/" + filename;
            
            // Check if file exists using stat
            struct stat st_file;
            if (stat(file_path.c_str(), &st_file) == 0 && S_ISREG(st_file.st_mode)) {
                if (!loadLanguageFromJson(file_path, language)) {
                    success = false;
                }
            }
        }
        
        return success;
    } catch (const std::exception& e) {
        std::cerr << "❌ Error loading translations from directory: " << e.what() << std::endl;
        return false;
    }
}

std::vector<std::string> LocalizationManager::getAllKeys() const {
    std::vector<std::string> keys;
    keys.reserve(translations_.size());
    
    for (const auto& pair : translations_) {
        keys.push_back(pair.first);
    }
    
    return keys;
}

LocalizationManager::Statistics LocalizationManager::getStatistics() const {
    Statistics stats;
    stats.total_keys = translations_.size();
    
    for (const auto& pair : translations_) {
        const Translation& translation = pair.second;
        bool has_russian = !translation.russian_text.empty();
        bool has_english = !translation.english_text.empty();
        
        if (has_russian && has_english) {
            stats.total_translations += 2;
        } else if (has_russian || has_english) {
            stats.total_translations += 1;
            stats.missing_translations += 1;
        } else {
            stats.missing_translations += 2;
        }
    }
    
    return stats;
}

void LocalizationManager::setLanguageChangeCallback(LanguageChangeCallback callback) {
    language_change_callback_ = std::move(callback);
}

void LocalizationManager::notifyLanguageChange() {
    if (language_change_callback_) {
        language_change_callback_(current_language_, current_language_);
    }
}

const char* LocalizationManager::translate_c_str(const std::string& key) const {
    // Use ring buffer to cache strings
    std::string result = translate(key);
    
    // Store in cache and return pointer to cached string
    string_cache_[string_cache_index_] = std::move(result);
    const char* ptr = string_cache_[string_cache_index_].c_str();
    
    // Advance index (ring buffer)
    string_cache_index_ = (string_cache_index_ + 1) % STRING_CACHE_SIZE;
    
    return ptr;
}

const char* LocalizationManager::translate_c_str(const std::string& key, const std::string& fallback) const {
    // Use ring buffer to cache strings
    std::string result = translate(key, fallback);
    
    // Store in cache and return pointer to cached string
    string_cache_[string_cache_index_] = std::move(result);
    const char* ptr = string_cache_[string_cache_index_].c_str();
    
    // Advance index (ring buffer)
    string_cache_index_ = (string_cache_index_ + 1) % STRING_CACHE_SIZE;
    
    return ptr;
}

void LocalizationManager::clearTranslationCache() {
    string_cache_.clear();
    string_cache_.resize(STRING_CACHE_SIZE);
    string_cache_index_ = 0;
}

bool LocalizationManager::loadLanguageFromJson(const std::string& file_path, Language language) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        std::cerr << "⚠ Cannot open translation file: " << file_path << std::endl;
        return false;
    }

    try {
        nlohmann::json json_data;
        file >> json_data;

        if (!json_data.contains("translations") || !json_data["translations"].is_object()) {
            std::cerr << "⚠ Invalid translation file format (missing 'translations' object): " << file_path << std::endl;
            return false;
        }

        size_t loaded_count = 0;
        for (const auto& item : json_data["translations"].items()) {
            std::string key = item.key();
            std::string value = item.value().get<std::string>();

            auto it = translations_.find(key);
            if (it != translations_.end()) {
                // Update existing translation entry
                switch (language) {
                    case Language::Russian:
                        it->second.russian_text = value;
                        break;
                    case Language::English:
                        it->second.english_text = value;
                        break;
                }
            } else {
                // Create new translation entry with empty text for other language
                Translation new_translation;
                new_translation.key = key;
                new_translation.russian_text = "";
                new_translation.english_text = "";
                
                switch (language) {
                    case Language::Russian:
                        new_translation.russian_text = value;
                        break;
                    case Language::English:
                        new_translation.english_text = value;
                        break;
                }
                
                translations_[key] = new_translation;
            }
            loaded_count++;
        }
        
        std::cout << "✓ Loaded " << loaded_count << " translations from " << file_path 
                  << " for language: " << getLanguageDisplayName(language) << std::endl;
        return true;
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "❌ JSON parsing error in " << file_path << ": " << e.what() << std::endl;
    }

    return false;
}

std::string LocalizationManager::getLanguageFileName(Language language) const {
    switch (language) {
        case Language::Russian:
            return "ru.json";
        case Language::English:
            return "en.json";
        default:
            return "unknown.json";
    }
}

std::string LocalizationManager::getLanguageDisplayName(Language language) const {
    switch (language) {
        case Language::Russian:
            return translate("language.russian", "Русский");
        case Language::English:
            return translate("language.english", "English");
        default:
            return translate("general.error", "Unknown");
    }
}

} // namespace ui
} // namespace llama_gui