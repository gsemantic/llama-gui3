#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <functional>

namespace llama_gui {
namespace ui {

// Built-in language enum (backward compatibility for the two default languages).
// Additional languages are supported dynamically via language codes
// (any <code>.json file placed in the i18n/ directory is picked up automatically).
enum class Language {
    Russian,    // Default language ("ru")
    English,    // Secondary language ("en")
    Unknown     // Dynamic language not covered by the built-in enum
};

// Information about an available language
struct LanguageInfo {
    std::string code;           // "ru", "en", "de", ...
    std::string display_name;   // "Русский", "English", ...
    std::string file_name;      // "ru.json", ...
    bool builtin = false;       // true for the two compiled-in languages
};

/**
 * Localization Manager - Handles text localization for the entire application
 * Supports Russian (default) and English with extensible architecture for future languages.
 *
 * To add a new language: create i18n/<code>.json with the same structure as ru.json/en.json.
 * It will be discovered automatically on startup (no code changes required).
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
    void setCurrentLanguage(const std::string& language_code);
    // Returns the built-in enum value for the current language
    // (Language::Unknown for dynamic languages not covered by the enum)
    Language getCurrentLanguage() const;
    // Returns the current language code (e.g. "ru", "en", "de")
    std::string getCurrentLanguageCode() const;
    // Returns the display name of the current language in its own language
    std::string getCurrentLanguageName() const;

    // List all available languages (discovered from i18n/ directory)
    std::vector<LanguageInfo> getLanguageInfos() const;
    std::vector<std::string> getAvailableLanguageCodes() const;
    std::vector<std::string> getAvailableLanguageNames() const;
    // Backward compatibility: returns only the two built-in languages
    std::vector<Language> getAvailableLanguages() const;

    bool isLanguageAvailable(const std::string& code) const;
    std::string getLanguageDisplayName(const std::string& code) const;

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
    void setTranslation(const std::string& language_code, const std::string& key, const std::string& text);

    // File operations
    bool loadTranslationsFromFile(const std::string& file_path);
    bool saveTranslationsToFile(const std::string& file_path) const;

    // Load all translations from JSON files in a directory (auto-discovers new languages)
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
    // Current language code (e.g. "ru", "en")
    std::string current_language_ = "ru";

    // All known languages in display order (ru, en, then any discovered ones)
    std::vector<LanguageInfo> languages_;
    std::unordered_map<std::string, size_t> language_index_;

    // All translations stored by language code, then by key
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> translations_;

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
    void registerLanguage(const std::string& code, const std::string& display_name, bool builtin);
    bool loadLanguageFromJson(const std::string& file_path, const std::string& language_code);
    bool loadLanguageFromJson(const std::string& file_path, Language language);
    std::string codeForLanguage(Language language) const;
    Language languageForCode(const std::string& code) const;
};

// Global localization manager instance
LocalizationManager& getLocalizationManager();

// Convenience macros for easy translation
// TR() and TRF() return const char* for direct ImGui compatibility
// Uses internal string cache to avoid dangling pointers
#define TR(key) (llama_gui::ui::getLocalizationManager().translate_c_str(key))
#define TRF(key, fallback) (llama_gui::ui::getLocalizationManager().translate_c_str(key, fallback))
#define CURRENT_LANGUAGE (llama_gui::ui::getLocalizationManager().getCurrentLanguage())
#define CURRENT_LANGUAGE_CODE (llama_gui::ui::getLocalizationManager().getCurrentLanguageCode())

} // namespace ui
} // namespace llama_gui
