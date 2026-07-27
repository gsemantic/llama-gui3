#pragma once

#include <string>
#include <vector>

namespace llama_gui {
namespace core {

class Stemmer {
public:
    // Detect if text contains Russian characters
    static bool is_russian(const std::string& text);

    // Detect if text contains English characters
    static bool is_english(const std::string& text);

    // Stem a Russian word to its root
    // Based on simplified Snowball rules for Russian
    static std::string stem_russian(const std::string& word);

    // Stem an English word to its root
    // Based on simplified Porter stemmer rules
    static std::string stem_english(const std::string& word);

    // Auto-detect language and stem
    static std::string stem(const std::string& word);

    // Generate morphological variants for a word (for query expansion)
    // Returns stems and common inflected forms
    static std::vector<std::string> expand_variants(const std::string& word);

private:
    // Russian suffix removal (longest match first)
    static std::string remove_russian_suffixes(const std::string& word);

    // English suffix removal (longest match first)
    static std::string remove_english_suffixes(const std::string& word);

    // Helper: check if word ends with suffix
    static bool ends_with(const std::string& word, const std::string& suffix);

    // Helper: remove suffix from word
    static std::string remove_suffix(const std::string& word, const std::string& suffix);

    // Helper: get UTF-8 character at position (for Russian)
    static std::string utf8_char_at(const std::string& text, size_t byte_pos);
};

} // namespace core
} // namespace llama_gui
