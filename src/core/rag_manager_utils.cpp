#include "../include/core/rag_manager.h"
#include "../include/core/stemmer.h"
#include <cmath>
#include <sstream>
#include <fstream>
#include <algorithm>

namespace llama_gui {
namespace core {

void RagManager::normalize_vector(std::vector<float>& vec) {
    float norm = 0.0f;
    for (float val : vec) {
        norm += val * val;
    }
    norm = std::sqrt(norm);
    if (norm > 0.0f) {
        for (float& val : vec) {
            val /= norm;
        }
    }
}

// ============================================================================
// Sentence boundary detection
// ============================================================================

struct SentenceBoundary {
    size_t position;   // Position after the delimiter
    char delimiter;    // The delimiter character
};

// Find all sentence boundaries in text
static std::vector<SentenceBoundary> find_sentence_boundaries(const std::string& text) {
    std::vector<SentenceBoundary> boundaries;

    for (size_t i = 0; i < text.size(); ) {
        unsigned char c = static_cast<unsigned char>(text[i]);

        // Check for multi-byte UTF-8 (Russian text = 2 bytes)
        if (c >= 0xC0) {
            // Skip UTF-8 multibyte character
            if ((c & 0xE0) == 0xC0) i += 2;
            else if ((c & 0xF0) == 0xE0) i += 3;
            else if ((c & 0xF8) == 0xF0) i += 4;
            else i++;
            continue;
        }

        // Check for sentence-ending punctuation
        if (c == '.' || c == '!' || c == '?') {
            // Handle ellipsis (...) - skip, not a sentence boundary
            if (c == '.' && i + 2 < text.size() && text[i+1] == '.' && text[i+2] == '.') {
                i += 3;
                continue;
            }

            // Find the end of this sentence (skip trailing spaces)
            size_t end = i + 1;

            // For Russian: "..." is not a boundary, but ". " is
            // For English: ". " or ".\n" is a boundary

            boundaries.push_back({end, c});
            i = end;
        }
        // Russian em-dash "—" (UTF-8: 0xE2 0x80 0x94) is a sentence boundary
        else if (c == 0xE2 && i + 2 < text.size() &&
                 static_cast<unsigned char>(text[i+1]) == 0x80 &&
                 static_cast<unsigned char>(text[i+2]) == 0x94) {
            boundaries.push_back({i + 3, '-'});
            i += 3;
        }
        // Regular dash "-" can also be a boundary in Russian (before enumeration)
        else if (c == '-' && i + 1 < text.size() &&
                 (text[i+1] == ' ' || text[i+1] == '\n')) {
            // Only treat as boundary if followed by space (not compound words)
            boundaries.push_back({i + 1, '-'});
            i++;
        }
        else {
            i++;
        }
    }

    return boundaries;
}

// Extract a sentence from text given boundaries
static std::string extract_sentence(const std::string& text,
                                     size_t start, size_t end) {
    // Trim trailing whitespace
    while (end > start && (text[end-1] == ' ' || text[end-1] == '\n' || text[end-1] == '\r')) {
        end--;
    }
    return text.substr(start, end - start);
}

std::vector<std::string> RagManager::split_into_chunks(const std::string& text, int max_tokens) {
    std::vector<std::string> chunks;

    if (text.empty()) return chunks;

    // Find all sentence boundaries
    auto boundaries = find_sentence_boundaries(text);

    // If no boundaries found, treat entire text as one sentence
    if (boundaries.empty()) {
        chunks.push_back(text);
        return chunks;
    }

    // Split text into sentences at boundaries
    std::vector<std::string> sentences;
    size_t prev_start = 0;

    for (const auto& boundary : boundaries) {
        std::string sentence = extract_sentence(text, prev_start, boundary.position);
        if (!sentence.empty()) {
            // The delimiter is already included in the extracted text
            sentences.push_back(sentence);
        }
        prev_start = boundary.position;
    }

    // Handle remaining text after last boundary
    if (prev_start < text.size()) {
        std::string remaining = extract_sentence(text, prev_start, text.size());
        if (!remaining.empty()) {
            sentences.push_back(remaining);
        }
    }

    // Group sentences into chunks based on max_tokens
    std::string current_chunk;

    for (const auto& sentence : sentences) {
        int current_tokens = count_tokens_approx(current_chunk);
        int sentence_tokens = count_tokens_approx(sentence);

        if (current_tokens + sentence_tokens <= max_tokens) {
            if (!current_chunk.empty()) {
                current_chunk += " ";
            }
            current_chunk += sentence;
        } else {
            // Save current chunk if non-empty
            if (!current_chunk.empty()) {
                chunks.push_back(current_chunk);
            }

            // If single sentence exceeds max_tokens, split it further
            if (sentence_tokens > max_tokens) {
                // Split long sentence by commas
                std::istringstream comma_stream(sentence);
                std::string segment;
                std::string sub_chunk;

                while (std::getline(comma_stream, segment, ',')) {
                    segment += ",";
                    int seg_tokens = count_tokens_approx(segment);

                    if (count_tokens_approx(sub_chunk) + seg_tokens <= max_tokens) {
                        if (!sub_chunk.empty()) sub_chunk += " ";
                        sub_chunk += segment;
                    } else {
                        if (!sub_chunk.empty()) chunks.push_back(sub_chunk);
                        sub_chunk = segment;
                    }
                }
                current_chunk = sub_chunk;
            } else {
                current_chunk = sentence;
            }
        }
    }

    // Add the last chunk
    if (!current_chunk.empty()) {
        chunks.push_back(current_chunk);
    }

    return chunks;
}

int RagManager::count_tokens_approx(const std::string& text) {
    if (text.empty()) return 0;

    // Word-based token estimation:
    // - Count words (split by whitespace)
    // - For Russian: each word ≈ 1.3 tokens on average (longer words)
    // - For English: each word ≈ 1.0 tokens
    // - Punctuation marks ≈ 0.3 tokens each

    int word_count = 0;
    int punct_count = 0;
    bool in_word = false;
    bool has_cyrillic = false;

    for (size_t i = 0; i < text.size(); ) {
        unsigned char c = static_cast<unsigned char>(text[i]);

        // Check for UTF-8 leading bytes
        if ((c & 0xC0) == 0xC0) {
            // This is a UTF-8 leading byte
            if (!in_word) {
                word_count++;
                in_word = true;
            }
            // Check if Cyrillic (0xD0-0xDF range)
            if (c >= 0xD0 && c <= 0xDF) {
                has_cyrillic = true;
            }
            // Skip the full UTF-8 character
            if ((c & 0xE0) == 0xC0) i += 2;
            else if ((c & 0xF0) == 0xE0) i += 3;
            else if ((c & 0xF8) == 0xF0) i += 4;
            else i++;
            continue;
        }

        // Check for UTF-8 continuation bytes
        if ((c & 0xC0) == 0x80) {
            i++;
            continue;
        }

        if (std::isalnum(c) || c == '-' || c == '_') {
            if (!in_word) {
                word_count++;
                in_word = true;
            }
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            in_word = false;
        } else if (c == '.' || c == ',' || c == ';' || c == ':' ||
                   c == '!' || c == '?' || c == '(' || c == ')') {
            punct_count++;
            in_word = false;
        }

        i++;
    }

    // Estimate: words + punctuation bonus
    // For Russian text, words are longer so slightly fewer tokens per word
    float words_per_token = has_cyrillic ? 0.85f : 1.0f;

    float estimated_f = word_count * words_per_token + punct_count * 0.3f;
    int estimated = static_cast<int>(estimated_f + 0.5f);  // Round instead of truncate
    return std::max(1, estimated);
}

std::string RagManager::get_file_extension(const std::string& file_path) {
    size_t pos = file_path.find_last_of('.');
    if (pos != std::string::npos) {
        return file_path.substr(pos);
    }
    return "";
}

std::string RagManager::read_txt_file(const std::string& file_path) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        return "";
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

} // namespace core
} // namespace llama_gui
