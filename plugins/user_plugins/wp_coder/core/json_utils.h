#pragma once

/*
 * json_utils.h — минимальные JSON-парсеры для ответов LLM (Фаза 4.6).
 *
 * Раньше дублировались: json_str/json_int в tool_protocol.cpp и
 * find_str/find_int в src/plugin_main.cpp (одинаковая логика, разные имена).
 * Здесь — единственная реализация (header-only, без nlohmann).
 */

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>

namespace coder {
namespace text {

inline void append_utf8(std::uint32_t cp, std::string& out) {
    if (cp <= 0x7F) {
        out += static_cast<char>(cp);
    } else if (cp <= 0x7FF) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

inline bool utf8_sequence_at(const std::string& s, size_t pos, size_t& length) {
    if (pos >= s.size()) return false;
    unsigned char first = static_cast<unsigned char>(s[pos]);
    if (first <= 0x7F) {
        length = 1;
        return true;
    }

    size_t n = 0;
    if (first >= 0xC2 && first <= 0xDF) n = 2;
    else if (first >= 0xE0 && first <= 0xEF) n = 3;
    else if (first >= 0xF0 && first <= 0xF4) n = 4;
    else return false;
    if (pos + n > s.size()) return false;

    for (size_t i = 1; i < n; ++i) {
        if ((static_cast<unsigned char>(s[pos + i]) & 0xC0) != 0x80)
            return false;
    }
    if (n == 3) {
        unsigned char second = static_cast<unsigned char>(s[pos + 1]);
        if ((first == 0xE0 && second < 0xA0) ||
            (first == 0xED && second > 0x9F))
            return false;
    }
    if (n == 4) {
        unsigned char second = static_cast<unsigned char>(s[pos + 1]);
        if ((first == 0xF0 && second < 0x90) ||
            (first == 0xF4 && second > 0x8F))
            return false;
    }
    length = n;
    return true;
}

inline bool is_valid_utf8(const std::string& s) {
    size_t pos = 0;
    while (pos < s.size()) {
        size_t length = 0;
        if (!utf8_sequence_at(s, pos, length)) return false;
        pos += length;
    }
    return true;
}

inline std::string sanitize_utf8(const std::string& s) {
    if (is_valid_utf8(s)) return s;
    std::string out;
    out.reserve(s.size());
    size_t pos = 0;
    while (pos < s.size()) {
        size_t length = 0;
        if (utf8_sequence_at(s, pos, length)) {
            out.append(s, pos, length);
            pos += length;
        } else {
            append_utf8(0xFFFD, out);
            ++pos;
        }
    }
    return out;
}

inline std::string utf8_prefix(const std::string& s, size_t max_bytes) {
    std::string clean = is_valid_utf8(s) ? s : sanitize_utf8(s);
    if (clean.size() <= max_bytes) return clean;
    size_t end = max_bytes;
    while (end > 0 &&
           (static_cast<unsigned char>(clean[end]) & 0xC0) == 0x80)
        --end;
    return clean.substr(0, end);
}

}
namespace json {

/* Экранирование строки для JSON (минимальный набор, для сериализации). */
inline std::string escape(const std::string& s) {
    std::string clean = text::is_valid_utf8(s) ? s : text::sanitize_utf8(s);
    std::string r;
    r.reserve(clean.size() + 8);
    for (char c : clean) {
        switch (c) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    r += buf;
                } else {
                    r += c;
                }
        }
    }
    return r;
}

/* Извлечь строковое значение поля по ключу (с обработкой экранов).
 * Возвращает пустую строку, если поле отсутствует. */
inline std::string str(const std::string& block, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = block.find(pat);
    if (p == std::string::npos) return "";
    size_t colon = block.find(':', p + pat.size());
    if (colon == std::string::npos) return "";
    size_t q1 = block.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    /* Ищем закрывающую кавычку с учётом экранированных \" и \\. */
    size_t q2 = q1 + 1;
    while (q2 < block.size()) {
        if (block[q2] == '\\') { q2 += 2; continue; }
        if (block[q2] == '"') break;
        ++q2;
    }
    if (q2 >= block.size()) return "";
    std::string v = block.substr(q1 + 1, q2 - q1 - 1);
    std::string r;
    auto hex_value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            char c = v[i + 1];
            if (c == 'n') { r += '\n'; i++; }
            else if (c == 't') { r += '\t'; i++; }
            else if (c == 'r') { r += '\r'; i++; }
            else if (c == '"') { r += '"'; i++; }
            else if (c == '\\') { r += '\\'; i++; }
            else if (c == '/' ) { r += '/'; i++; }
            else if (c == 'u' && i + 5 < v.size()) {
                uint32_t cp = 0;
                bool valid = true;
                for (size_t j = 0; j < 4; ++j) {
                    int h = hex_value(v[i + 2 + j]);
                    if (h < 0) { valid = false; break; }
                    cp = (cp << 4) | static_cast<uint32_t>(h);
                }
                if (!valid) {
                    r += v[i++];
                    continue;
                }
                i += 5;
                if (cp >= 0xD800 && cp <= 0xDBFF &&
                    i + 5 < v.size() && v[i] == '\\' && v[i + 1] == 'u') {
                    uint32_t low = 0;
                    bool low_valid = true;
                    for (size_t j = 0; j < 4; ++j) {
                        int h = hex_value(v[i + 2 + j]);
                        if (h < 0) { low_valid = false; break; }
                        low = (low << 4) | static_cast<uint32_t>(h);
                    }
                    if (low_valid && low >= 0xDC00 && low <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        i += 6;
                    }
                }
                if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
                text::append_utf8(cp, r);
            }
            else r += v[i];
        } else r += v[i];
    }
    return text::is_valid_utf8(r) ? r : text::sanitize_utf8(r);
}

/* Извлечь целочисленное значение поля по ключу. 0, если отсутствует. */
inline int int_(const std::string& block, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = block.find(pat);
    if (p == std::string::npos) return 0;
    size_t colon = block.find(':', p + pat.size());
    if (colon == std::string::npos) return 0;
    size_t s = colon + 1;
    while (s < block.size() && (block[s] == ' ' || block[s] == '\t')) ++s;
    int n = 0;
    while (s < block.size() && std::isdigit(static_cast<unsigned char>(block[s]))) {
        n = n * 10 + (block[s] - '0'); ++s;
    }
    return n;
}

} // namespace json
} // namespace coder