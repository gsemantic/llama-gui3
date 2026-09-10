#include "tool_protocol.h"

#include <sstream>
#include <cctype>

namespace coder {

std::string extract_action(const std::string& text, std::string& rest) {
    rest = text;
    size_t a = text.find("```wp_action");
    if (a == std::string::npos) {
        a = text.find("```json");
        if (a != std::string::npos) {
            /* JSON-блок: ищем { ... } внутри обрамления. */
            size_t body = text.find('{', a);
            if (body == std::string::npos) return "";
            size_t b = text.find("```", body);
            if (b == std::string::npos) return "";
            std::string block = text.substr(body, b - body);
            rest = text.substr(0, a) + text.substr(b + 3);
            return block;
        }
        a = text.find("```\nwp_action");
        if (a == std::string::npos) {
            a = text.find("```");
            if (a == std::string::npos) return "";
            size_t body_check = text.find('\n', a);
            if (body_check == std::string::npos) return "";
            size_t wp = text.find("wp_action", body_check + 1);
            size_t close = text.find("```", body_check + 1);
            if (wp == std::string::npos || (close != std::string::npos && wp > close)) return "";
        }
    }
    size_t body = text.find('\n', a);
    if (body == std::string::npos) return "";
    size_t b = text.find("```", body);
    if (b == std::string::npos) return "";
    std::string block = text.substr(body + 1, b - body - 1);
    rest = text.substr(0, a) + text.substr(b + 3);
    return block;
}

namespace {

/* Извлечь строковое значение JSON-поля по ключу (с экранированием). */
std::string json_str(const std::string& block, const char* key) {
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

int json_int(const std::string& block, const char* key) {
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

} // namespace

bool parse_action(const std::string& block, Action& a) {
    /* JSON-протокол: {"tool":"...","path":"...","k":N,...} (Фаза B3). */
    size_t brace = block.find('{');
    if (brace != std::string::npos && block.find('}') != std::string::npos) {
        a.tool = json_str(block, "tool");
        if (!a.tool.empty()) {
            a.path = json_str(block, "path");
            a.root = json_str(block, "root");
            a.query = json_str(block, "query");
            a.pattern = json_str(block, "pattern");
            a.cli = json_str(block, "cli");
            a.url = json_str(block, "url");
            a.content = json_str(block, "content");
            int k = json_int(block, "k");
            if (k > 0) a.k = k;
            return true;
        }
        /* Если tool нет — это не наш JSON (обычный ответ), падаем в legacy. */
    }

    std::istringstream iss(block);
    std::string line;
    bool in_content = false;
    while (std::getline(iss, line)) {
        while (!line.empty() && line.back() == '\r') line.pop_back();

        if (in_content) {
            if (line.find("CONTENT_END") != std::string::npos) { in_content = false; continue; }
            if (!a.content.empty()) a.content += '\n';
            a.content += line;
            continue;
        }
        if (line.find("CONTENT_BEGIN") != std::string::npos) { in_content = true; continue; }
        auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);
        auto trim = [](std::string& s) {
            while (!s.empty() && s.back() == '\r') s.pop_back();
            size_t b = s.find_first_not_of(" \t");
            size_t e = s.find_last_not_of(" \t");
            s = (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
        };
        trim(key); trim(val);
        if (key == "TOOL") a.tool = val;
        else if (key == "PATH") a.path = val;
        else if (key == "ROOT") a.root = val;
        else if (key == "QUERY") a.query = val;
        else if (key == "PATTERN") a.pattern = val;
        else if (key == "CLI") a.cli = val;
        else if (key == "URL") a.url = val;
        else if (key == "K") a.k = atoi(val.c_str());
        else if (key == "CONTENT_BEGIN") in_content = true;
    }
    return !a.tool.empty();
}

} // namespace coder