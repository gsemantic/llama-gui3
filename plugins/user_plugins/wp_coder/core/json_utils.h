#pragma once

/*
 * json_utils.h — минимальные JSON-парсеры для ответов LLM (Фаза 4.6).
 *
 * Раньше дублировались: json_str/json_int в tool_protocol.cpp и
 * find_str/find_int в src/plugin_main.cpp (одинаковая логика, разные имена).
 * Здесь — единственная реализация (header-only, без nlohmann).
 */

#include <cctype>
#include <string>

namespace coder {
namespace json {

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
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            char c = v[i + 1];
            if (c == 'n') { r += '\n'; i++; }
            else if (c == 't') { r += '\t'; i++; }
            else if (c == 'r') { r += '\r'; i++; }
            else if (c == '"') { r += '"'; i++; }
            else if (c == '\\') { r += '\\'; i++; }
            else r += v[i];
        } else r += v[i];
    }
    return r;
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