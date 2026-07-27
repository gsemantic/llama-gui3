#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <functional>

namespace llama_gui {
namespace ui {

// Supported languages enum
enum class Language {
    Russian,    // Default language
    English     // Secondary language
};

// Translation structure
struct Translation {
    std::string key;
    std::string russian_text;
    std::string english_text;
};

/**
 * Localization Manager - Handles text localization for the entire application
 * Supports Russian (default) and English with extensible architecture for future languages
 */
class LocalizationManager {
public:
    // Constructor and destructor
    LocalizationManager();
    ~LocalizationManager() = default;

    // Delete copy and move operations
    LocalizationManager(const LocalizationManager&) = delete;
    LocalizationManager& operator=(const LocalizationManager&) = delete;
    LocalizationManager(LocalizationManager&&) = delete;
    LocalizationManager& operator=(LocalizationManager&&) = delete;

    // Language management
    void setCurrentLanguage(Language language);
    Language getCurrentLanguage() const { return current_language_; }
    std::string getCurrentLanguageName() const;
    std::vector<std::string> getAvailableLanguageNames() const;
    std::vector<Language> getAvailableLanguages() const;

    // Text translation
    std::string translate(const std::string& key) const;
    std::string translate(const std::string& key, const std::string& fallback) const;
    
    // Convenience methods for common patterns
    std::string tr(const std::string& key) const { return translate(key); }
    std::string tr(const std::string& key, const std::string& fallback) const { return translate(key, fallback); }

    // Check if translation exists
    bool hasTranslation(const std::string& key) const;

    // Add/update translations programmatically
    void addTranslation(const std::string& key, const std::string& russian_text, const std::string& english_text);
    void updateTranslation(Language language, const std::string& key, const std::string& text);

    // File operations
    bool loadTranslationsFromFile(const std::string& file_path);
    bool saveTranslationsToFile(const std::string& file_path) const;

    // Load all translations from JSON files in a directory
    bool loadTranslationsFromDirectory(const std::string& directory_path);

    // Get all translation keys
    std::vector<std::string> getAllKeys() const;

    // Get translation statistics
    struct Statistics {
        size_t total_keys = 0;
        size_t missing_translations = 0;
        size_t total_translations = 0;
    };
    
    Statistics getStatistics() const;

    // Callback for language changes
    using LanguageChangeCallback = std::function<void(Language new_language, Language old_language)>;
    void setLanguageChangeCallback(LanguageChangeCallback callback);
    
    // Force refresh all UI components when language changes
    void notifyLanguageChange();

    // Get translation as const char* (safe for ImGui - uses internal cache)
    const char* translate_c_str(const std::string& key) const;
    const char* translate_c_str(const std::string& key, const std::string& fallback) const;

    // Clear translation cache (call after language change)
    void clearTranslationCache();

private:
    // Current language
    Language current_language_ = Language::Russian;

    // All translations stored by key
    std::unordered_map<std::string, Translation> translations_;

    // Language change callback
    LanguageChangeCallback language_change_callback_;

    // String cache for safe const char* returns (thread-local)
    // Uses a ring buffer to store recent translations
    mutable std::vector<std::string> string_cache_;
    mutable size_t string_cache_index_ = 0;
    static constexpr size_t STRING_CACHE_SIZE = 256;

    // Internal methods
    void initializeDefaultTranslations();
    void initializeMenuTranslations();
    void initializeSettingsTranslations();
    void initializeCommonTranslations();
    bool loadLanguageFromJson(const std::string& file_path, Language language);
    std::string getLanguageFileName(Language language) const;
    std::string getLanguageDisplayName(Language language) const;
};

// Global localization manager instance
LocalizationManager& getLocalizationManager();

// Convenience macros for easy translation
// TR() and TRF() return const char* for direct ImGui compatibility
// Uses internal string cache to avoid dangling pointers
#define TR(key) (llama_gui::ui::getLocalizationManager().translate_c_str(key))
#define TRF(key, fallback) (llama_gui::ui::getLocalizationManager().translate_c_str(key, fallback))
#define TRFMT(key, ...) (llama_gui::ui::getLocalizationManager().translate_c_str(key), __VA_ARGS__)
#define CURRENT_LANGUAGE (llama_gui::ui::getLocalizationManager().getCurrentLanguage())

} // namespace ui
} // namespace llama_gui