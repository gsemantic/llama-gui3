#include "../include/core/stemmer.h"
#include <algorithm>
#include <unordered_set>

namespace llama_gui {
namespace core {

bool Stemmer::is_russian(const std::string& text) {
    for (size_t i = 0; i < text.size(); ) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= 0xC0) {  // UTF-8 multibyte (Russian letters are 2-byte: 0xD0-0xD1)
            return true;
        }
        i++;
    }
    return false;
}

bool Stemmer::is_english(const std::string& text) {
    for (char c : text) {
        if (c >= 'a' && c <= 'z') return true;
        if (c >= 'A' && c <= 'Z') return true;
    }
    return false;
}

bool Stemmer::ends_with(const std::string& word, const std::string& suffix) {
    if (suffix.size() > word.size()) return false;
    return word.compare(word.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string Stemmer::remove_suffix(const std::string& word, const std::string& suffix) {
    if (ends_with(word, suffix)) {
        return word.substr(0, word.size() - suffix.size());
    }
    return word;
}

// ============================================================================
// Russian Stemmer (simplified Snowball rules)
// ============================================================================
// Order matters: longer suffixes first, then shorter ones.
// We try to preserve at least 3 characters of the word stem.

std::string Stemmer::remove_russian_suffixes(const std::string& word) {
    // Russian suffixes ordered by length (longest first)
    // These are the most common Russian morphological endings
    static const std::vector<std::pair<std::string, std::string>> suffixes = {
        // 4-character suffixes (most specific)
        {"ого", ""}, {"ому", ""}, {"ему", ""},
        {"аться", ""}, {"иться", ""}, {"яться", ""}, {"оться", ""}, {"уться", ""},
        // 3-character suffixes
        {"ать", ""}, {"ять", ""}, {"ить", ""}, {"ыть", ""}, {"оть", ""}, {"уть", ""},
        {"ение", ""}, {"ание", ""}, {"ятие", ""},
        {"тся", ""}, {"ться", ""},
        {"ейш", ""}, {"айш", ""},
        // 2-character suffixes
        {"ов", ""}, {"ев", ""}, {"ей", ""}, {"ой", ""}, {"ий", ""}, {"ый", ""}, {"ая", ""}, {"яя", ""}, {"ое", ""}, {"ее", ""}, {"ые", ""}, {"ие", ""},
        {"ах", ""}, {"ях", ""}, {"ам", ""}, {"ям", ""},
        {"ом", ""}, {"ем", ""},
        {"ую", ""}, {"юю", ""}, {"ает", ""}, {"яет", ""},
        {"ей", ""}, {"ой", ""},
        // 1-character suffixes (least specific) - order matters!
        {"а", ""}, {"е", ""}, {"и", ""}, {"о", ""}, {"у", ""}, {"ы", ""}, {"ь", ""}, {"я", ""}, {"ю", ""},
    };

    std::string result = word;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Don't stem words shorter than 4 characters
    if (result.length() < 4) return result;

    for (const auto& [suffix, replacement] : suffixes) {
        if (ends_with(result, suffix)) {
            std::string stemmed = remove_suffix(result, suffix) + replacement;
            // Ensure minimum stem length of 3
            if (stemmed.length() >= 3) {
                return stemmed;
            }
        }
    }

    return result;
}

std::string Stemmer::stem_russian(const std::string& word) {
    std::string lower = word;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return remove_russian_suffixes(lower);
}

// ============================================================================
// English Stemmer (simplified Porter stemmer rules)
// ============================================================================

std::string Stemmer::remove_english_suffixes(const std::string& word) {
    static const std::vector<std::pair<std::string, std::string>> suffixes = {
        // 5-character suffixes
        {"ation", "at"}, {"iness", "i"}, {"ness", ""}, {"ment", ""},
        // 4-character suffixes
        {"able", ""}, {"ible", ""}, {"tion", "t"}, {"sion", "s"},
        {"ling", ""}, {"ally", ""}, {"ized", "ize"}, {"ised", "ise"},
        {"ency", ""}, {"ancy", ""}, {"ical", ""},
        // 3-character suffixes
        {"ing", ""}, {"ful", ""}, {"ous", ""}, {"ive", ""}, {"ity", ""},
        {"ent", ""}, {"ant", ""}, {"ism", ""}, {"ist", ""}, {"ize", ""},
        {"ate", ""}, {"ify", ""},
        // 2-character suffixes
        {"ly", ""}, {"er", ""}, {"ed", ""}, {"es", ""}, {"al", ""},
        {"en", ""}, {"an", ""}, {"or", ""},
        // 1-character suffixes
        {"s", ""},
    };

    std::string result = word;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Don't stem words shorter than 4 characters
    if (result.length() < 4) return result;

    for (const auto& [suffix, replacement] : suffixes) {
        if (ends_with(result, suffix)) {
            std::string stemmed = remove_suffix(result, suffix) + replacement;
            // Ensure minimum stem length of 3
            if (stemmed.length() >= 3) {
                return stemmed;
            }
        }
    }

    return result;
}

std::string Stemmer::stem_english(const std::string& word) {
    std::string lower = word;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return remove_english_suffixes(lower);
}

// ============================================================================
// Auto-detect and stem
// ============================================================================

std::string Stemmer::stem(const std::string& word) {
    if (is_russian(word)) {
        return stem_russian(word);
    }
    return stem_english(word);
}

// ============================================================================
// Morphological variants for query expansion
// ============================================================================

std::vector<std::string> Stemmer::expand_variants(const std::string& word) {
    std::vector<std::string> variants;
    std::string lower = word;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    variants.push_back(lower);

    if (is_russian(lower)) {
        // Russian: add common case endings
        static const std::vector<std::string> ru_endings = {
            "", "а", "у", "ом", "е", "ы", "ами", "ов", "ев", "ей"
        };
        for (const auto& ending : ru_endings) {
            std::string variant = lower + ending;
            if (variant != lower && variant.length() >= 3) {
                variants.push_back(variant);
            }
        }
    } else if (is_english(lower)) {
        // English: add common inflections
        static const std::vector<std::string> en_endings = {
            "", "s", "es", "ed", "ing", "ly", "er", "est"
        };
        for (const auto& ending : en_endings) {
            std::string variant = lower + ending;
            if (variant != lower && variant.length() >= 3) {
                variants.push_back(variant);
            }
        }
    }

    return variants;
}

} // namespace core
} // namespace llama_gui
