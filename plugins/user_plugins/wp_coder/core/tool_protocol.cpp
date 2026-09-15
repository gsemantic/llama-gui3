#include "tool_protocol.h"
#include "json_utils.h"

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
            if (a == std::string::npos) {
                /* Fallback: unfenced wp_action или JSON-блок.
                 * Модели иногда пишут wp_action без обратных кавычек:
                 *   wp_action\nTOOL: ...\nPATH: ...\n
                 * или JSON без обрамления:
                 *   {"tool": "read_file", "path": "..."}  */
                size_t uf_wp = text.find("wp_action\n");
                if (uf_wp != std::string::npos) {
                    size_t body_start = text.find('\n', uf_wp);
                    if (body_start == std::string::npos) return "";
                    /* Ищем конец: двойной newline или конец текста. */
                    size_t body_end = text.find("\n\n", body_start + 1);
                    if (body_end == std::string::npos) body_end = text.size();
                    std::string block = text.substr(body_start + 1, body_end - body_start - 1);
                    rest = text.substr(0, uf_wp) + text.substr(body_end);
                    return block;
                }
                /* Unfenced JSON: строка начинается с { и содержит "tool": */
                size_t uf_json = text.find("{\"tool\"");
                if (uf_json == std::string::npos) return "";
                size_t json_end = text.find('}', uf_json);
                if (json_end == std::string::npos) return "";
                std::string block = text.substr(uf_json, json_end - uf_json + 1);
                rest = text.substr(0, uf_json) + text.substr(json_end + 1);
                return block;
            }

            /* Generic ``` fence (без "json" или "wp_action" суффикса).
             * Модели (особенно qwen3-30b) иногда пишут:
             *   ```{"tool": "read_file", "path": "main.py"}```
             * или:
             *   ```
             *   {"tool": "read_file", "path": "main.py"}
             *   ```
             * Ищем JSON { ... } внутри fence. */
            size_t close = text.find("```", a + 3);
            if (close != std::string::npos) {
                /* Контент между открывающим и закрывающим ```. */
                std::string fence = text.substr(a + 3, close - a - 3);
                /* Убираем ведущие пробелы/newlines. */
                size_t content_start = fence.find_first_not_of(" \t\n\r");
                if (content_start != std::string::npos && fence[content_start] == '{') {
                    /* JSON внутри generic fence — извлекаем. */
                    size_t json_end = fence.rfind('}');
                    if (json_end != std::string::npos && json_end >= content_start) {
                        std::string block = fence.substr(content_start, json_end - content_start + 1);
                        rest = text.substr(0, a) + text.substr(close + 3);
                        return block;
                    }
                }
                /* Не JSON — проверяем wp_action внутри fence. */
                size_t wp = fence.find("wp_action");
                if (wp != std::string::npos) {
                    size_t wp_body = fence.find('\n', wp);
                    if (wp_body != std::string::npos) {
                        std::string block = fence.substr(wp_body + 1);
                        /* Убираем trailing whitespace. */
                        while (!block.empty() && (block.back() == '\n' || block.back() == '\r'))
                            block.pop_back();
                        rest = text.substr(0, a) + text.substr(close + 3);
                        return block;
                    }
                }
            }

            /* Ни JSON, ни wp_action внутри ``` — нет инструмента. */
            return "";
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

bool parse_action(const std::string& block, Action& a) {
    /* JSON-протокол: {"tool":"...","path":"...","k":N,...} (Фаза B3). */
    size_t brace = block.find('{');
    if (brace != std::string::npos && block.find('}') != std::string::npos) {
        a.tool = json::str(block, "tool");
        if (!a.tool.empty()) {
            a.path = json::str(block, "path");
            a.root = json::str(block, "root");
            a.query = json::str(block, "query");
            a.pattern = json::str(block, "pattern");
            a.cli = json::str(block, "cli");
            a.url = json::str(block, "url");
            a.content = json::str(block, "content");
            int k = json::int_(block, "k");
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