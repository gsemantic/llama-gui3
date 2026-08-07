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

    // Built-in default translations (registers "ru" and "en")
    initializeDefaultTranslations();

    // Load translations from JSON files, trying several possible i18n paths.
    // Any <code>.json file in i18n/ is discovered automatically (new languages need no code changes).
    std::vector<std::string> possible_paths = {
        "i18n",
        "../i18n",
        "../../i18n",
        "../../../i18n",
        "translations",
        "../translations",
        "../../translations"
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
    // Initialize translations through the modular methods
    initializeMenuTranslations();
    initializeSettingsTranslations();
    initializeCommonTranslations();

    std::cout << "✓ Localization manager initialized with " << translations_["ru"].size()
              << " default translations" << std::endl;
}

// =========================================================================
// Language registry
// =========================================================================

bool isBuiltinCode(const std::string& code) {
    return code == "ru" || code == "en";
}

std::string defaultDisplayName(const std::string& code) {
    if (code == "ru") return "Русский";
    if (code == "en") return "English";
    return code;
}

void LocalizationManager::registerLanguage(const std::string& code, const std::string& display_name, bool builtin) {
    if (code.empty()) return;

    auto it = language_index_.find(code);
    if (it != language_index_.end()) {
        // Update display name if provided
        if (!display_name.empty()) {
            languages_[it->second].display_name = display_name;
        }
        return;
    }

    LanguageInfo info;
    info.code = code;
    info.display_name = display_name.empty() ? defaultDisplayName(code) : display_name;
    info.file_name = code + ".json";
    info.builtin = builtin;
    language_index_[code] = languages_.size();
    languages_.push_back(std::move(info));
}

std::string LocalizationManager::codeForLanguage(Language language) const {
    switch (language) {
        case Language::Russian: return "ru";
        case Language::English: return "en";
        default: return "";
    }
}

Language LocalizationManager::languageForCode(const std::string& code) const {
    if (code == "ru") return Language::Russian;
    if (code == "en") return Language::English;
    return Language::Unknown;
}

// =========================================================================
// Language management
// =========================================================================

void LocalizationManager::setCurrentLanguage(Language language) {
    setCurrentLanguage(codeForLanguage(language));
}

void LocalizationManager::setCurrentLanguage(const std::string& language_code) {
    // Ignore unknown languages
    if (language_code.empty() || !language_index_.count(language_code)) {
        return;
    }

    if (current_language_ != language_code) {
        std::string old_code = current_language_;
        Language old_language = languageForCode(old_code);
        current_language_ = language_code;

        std::cout << "✓ Language changed from " << getLanguageDisplayName(old_code)
                  << " to " << getLanguageDisplayName(language_code) << std::endl;

        // Clear translation cache to avoid stale strings
        clearTranslationCache();

        // Notify about language change
        if (language_change_callback_) {
            language_change_callback_(languageForCode(language_code), old_language);
        }
    }
}

Language LocalizationManager::getCurrentLanguage() const {
    return languageForCode(current_language_);
}

std::string LocalizationManager::getCurrentLanguageCode() const {
    return current_language_;
}

std::string LocalizationManager::getCurrentLanguageName() const {
    return getLanguageDisplayName(current_language_);
}

std::vector<LanguageInfo> LocalizationManager::getLanguageInfos() const {
    return languages_;
}

std::vector<std::string> LocalizationManager::getAvailableLanguageCodes() const {
    std::vector<std::string> codes;
    codes.reserve(languages_.size());
    for (const auto& lang : languages_) {
        codes.push_back(lang.code);
    }
    return codes;
}

std::vector<std::string> LocalizationManager::getAvailableLanguageNames() const {
    std::vector<std::string> names;
    names.reserve(languages_.size());
    for (const auto& lang : languages_) {
        names.push_back(lang.display_name);
    }
    return names;
}

std::vector<Language> LocalizationManager::getAvailableLanguages() const {
    // Backward compatibility: only the two built-in languages
    return { Language::Russian, Language::English };
}

bool LocalizationManager::isLanguageAvailable(const std::string& code) const {
    return language_index_.count(code) != 0;
}

std::string LocalizationManager::getLanguageDisplayName(const std::string& code) const {
    auto it = language_index_.find(code);
    if (it != language_index_.end()) {
        return languages_[it->second].display_name;
    }
    return defaultDisplayName(code);
}

// =========================================================================
// Text translation
// =========================================================================

std::string LocalizationManager::translate(const std::string& key) const {
    // Look up in the current language
    auto lang_it = translations_.find(current_language_);
    if (lang_it != translations_.end()) {
        auto it = lang_it->second.find(key);
        if (it != lang_it->second.end() && !it->second.empty()) {
            return it->second;
        }
    }

    // Fallback to Russian (default language)
    if (current_language_ != "ru") {
        auto ru_it = translations_.find("ru");
        if (ru_it != translations_.end()) {
            auto it = ru_it->second.find(key);
            if (it != ru_it->second.end() && !it->second.empty()) {
                return it->second;
            }
        }
    }

    // Return key if translation not found (useful for development)
    return key;
}

std::string LocalizationManager::translate(const std::string& key, const std::string& fallback) const {
    // Look up in the current language
    auto lang_it = translations_.find(current_language_);
    if (lang_it != translations_.end()) {
        auto it = lang_it->second.find(key);
        if (it != lang_it->second.end() && !it->second.empty()) {
            return it->second;
        }
    }

    // Fallback to Russian (default language)
    if (current_language_ != "ru") {
        auto ru_it = translations_.find("ru");
        if (ru_it != translations_.end()) {
            auto it = ru_it->second.find(key);
            if (it != ru_it->second.end() && !it->second.empty()) {
                return it->second;
            }
        }
    }

    return fallback;
}

bool LocalizationManager::hasTranslation(const std::string& key) const {
    // Check current language and Russian fallback
    for (const auto& code : { current_language_, std::string("ru") }) {
        auto lang_it = translations_.find(code);
        if (lang_it != translations_.end()) {
            auto it = lang_it->second.find(key);
            if (it != lang_it->second.end() && !it->second.empty()) {
                return true;
            }
        }
    }
    return false;
}

void LocalizationManager::setTranslation(const std::string& language_code, const std::string& key, const std::string& text) {
    if (language_code.empty()) return;
    registerLanguage(language_code, "", isBuiltinCode(language_code));
    translations_[language_code][key] = text;
}

void LocalizationManager::addTranslation(const std::string& key, const std::string& russian_text, const std::string& english_text) {
    setTranslation("ru", key, russian_text);
    setTranslation("en", key, english_text);
}

void LocalizationManager::updateTranslation(Language language, const std::string& key, const std::string& text) {
    setTranslation(codeForLanguage(language), key, text);
}

// =========================================================================
// File operations
// =========================================================================

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

        auto ru_it = translations_.find("ru");
        if (ru_it != translations_.end()) {
            for (const auto& pair : ru_it->second) {
                nlohmann::json item;
                item["key"] = pair.first;
                item["russian"] = pair.second;

                auto en_lang_it = translations_.find("en");
                std::string en_text;
                if (en_lang_it != translations_.end()) {
                    auto en_it = en_lang_it->second.find(pair.first);
                    if (en_it != en_lang_it->second.end()) {
                        en_text = en_it->second;
                    }
                }
                item["english"] = en_text;
                translations_array.push_back(item);
            }
        }

        json_data["translations"] = translations_array;

        file << json_data.dump(2) << std::endl;
        std::cout << "✓ Saved " << translations_array.size() << " translations to " << file_path << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "❌ Error saving translations to " << file_path << ": " << e.what() << std::endl;
    }

    return false;
}

bool LocalizationManager::loadTranslationsFromDirectory(const std::string& directory_path) {
    // Check if directory exists using stat
    struct stat st;
    if (stat(directory_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        return false;
    }

    // Collect all *.json files
    std::vector<std::string> files;
    DIR* dir = opendir(directory_path.c_str());
    if (!dir) return false;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0) {
            files.push_back(name);
        }
    }
    closedir(dir);

    if (files.empty()) return false;

    // Sort so ru.json and en.json load first (stable order), others alphabetically
    std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) {
        auto rank = [](const std::string& f) {
            if (f == "ru.json") return 0;
            if (f == "en.json") return 1;
            return 2;
        };
        int ra = rank(a), rb = rank(b);
        if (ra != rb) return ra < rb;
        return a < b;
    });

    bool success = false;
    for (const auto& fname : files) {
        std::string file_path = directory_path + "/" + fname;

        struct stat st_file;
        if (stat(file_path.c_str(), &st_file) == 0 && S_ISREG(st_file.st_mode)) {
            // Language code is detected from the JSON "language" field
            if (loadLanguageFromJson(file_path, "")) {
                success = true;
            }
        }
    }

    return success;
}

std::vector<std::string> LocalizationManager::getAllKeys() const {
    std::vector<std::string> keys;
    std::unordered_map<std::string, bool> seen;
    for (const auto& lang_pair : translations_) {
        for (const auto& kv : lang_pair.second) {
            if (!seen.count(kv.first)) {
                seen[kv.first] = true;
                keys.push_back(kv.first);
            }
        }
    }
    return keys;
}

LocalizationManager::Statistics LocalizationManager::getStatistics() const {
    Statistics stats;

    // Count keys across all registered languages
    std::vector<std::string> keys = getAllKeys();
    stats.total_keys = keys.size();

    std::vector<std::string> codes = getAvailableLanguageCodes();
    if (codes.empty()) return stats;

    for (const auto& key : keys) {
        size_t present = 0;
        for (const auto& code : codes) {
            auto lang_it = translations_.find(code);
            if (lang_it != translations_.end()) {
                auto it = lang_it->second.find(key);
                if (it != lang_it->second.end() && !it->second.empty()) {
                    present++;
                }
            }
        }
        stats.total_translations += present;
        if (present < codes.size()) {
            stats.missing_translations += (codes.size() - present);
        }
    }

    return stats;
}

void LocalizationManager::setLanguageChangeCallback(LanguageChangeCallback callback) {
    language_change_callback_ = std::move(callback);
}

void LocalizationManager::notifyLanguageChange() {
    if (language_change_callback_) {
        Language lang = languageForCode(current_language_);
        language_change_callback_(lang, lang);
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

// =========================================================================
// JSON language loading
// =========================================================================

bool LocalizationManager::loadLanguageFromJson(const std::string& file_path, const std::string& language_code) {
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

        // Determine language code: explicit param > JSON "language" field > filename
        std::string code = language_code;
        if (code.empty() && json_data.contains("language") && json_data["language"].is_string()) {
            code = json_data["language"].get<std::string>();
        }
        if (code.empty()) {
            std::string fname = file_path;
            size_t slash = fname.find_last_of("/\\");
            if (slash != std::string::npos) fname = fname.substr(slash + 1);
            if (fname.size() > 5) code = fname.substr(0, fname.size() - 5);
        }
        if (code.empty()) {
            std::cerr << "⚠ Cannot determine language code for: " << file_path << std::endl;
            return false;
        }

        // Register the language (this is what makes new languages appear in the selector)
        std::string display_name;
        if (json_data.contains("language_name") && json_data["language_name"].is_string()) {
            display_name = json_data["language_name"].get<std::string>();
        }
        registerLanguage(code, display_name, isBuiltinCode(code));

        auto& lang_map = translations_[code];
        size_t loaded_count = 0;
        for (const auto& item : json_data["translations"].items()) {
            std::string key = item.key();
            std::string value = item.value().get<std::string>();
            lang_map[key] = value;
            loaded_count++;
        }

        std::cout << "✓ Loaded " << loaded_count << " translations from " << file_path
                  << " for language: " << code << std::endl;
        return true;
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "❌ JSON parsing error in " << file_path << ": " << e.what() << std::endl;
    }

    return false;
}

bool LocalizationManager::loadLanguageFromJson(const std::string& file_path, Language language) {
    return loadLanguageFromJson(file_path, codeForLanguage(language));
}

} // namespace ui
} // namespace llama_gui
