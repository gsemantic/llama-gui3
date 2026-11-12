#include "tool_protocol.h"
#include "json_utils.h"
#include "module_api.h"

#include <sstream>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace coder {

namespace {

/* Ключи аргументов — единый список для обоих направлений разбора
 * (JSON → Action и legacy → JsonValue). Раньше он был продублирован
 * в вызовах json::str(...) и в legacy-разборе, и это была причина
 * расхождений: «добавил параметр в одном месте — забыл в другом». */
struct ArgKey {
    const char* name;
    std::string Action::*member;
};

const ArgKey g_action_keys[] = {
    {"path",    &Action::path},
    {"root",    &Action::root},
    {"query",   &Action::query},
    {"pattern", &Action::pattern},
    {"content", &Action::content},
    {"cli",     &Action::cli},
    {"url",     &Action::url},
};

void trim_ws(std::string& s) {
    while (!s.empty() && s.back() == '\r') s.pop_back();
    size_t b = s.find_first_not_of(" \t");
    size_t e = s.find_last_not_of(" \t");
    s = (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
}

/* Конец legacy-блока строк «KEY: value», начинающегося с block_start.
 *
 * Пустая строка — граница только для блока-заголовка (TOOL/PATH без
 * CONTENT). Блок с CONTENT_BEGIN пустой строкой внутри кода не кончается:
 * резать по ней — значит записать в файл усечённое содержимое, а остаток
 * кода отдать в текст ответа. Наблюдение на qwen-flash (DashScope,
 * 2026-11): write_file с кодом калькулятора оборвался на первой пустой
 * строке — в файл ушло 2 строки импортов (51 байт), остальные 4190
 * символов стали текстом хода, и цикл закрыл задачу как выполненную.
 *
 * Правило: в вырезанном куске CONTENT_BEGIN есть, а CONTENT_END нет —
 * блок дотягивается до строки CONTENT_END в ПОЛНОМ тексте, а без неё —
 * до конца текста. Хвостовая проза после незакрытого блока уйдёт в
 * контент — это осознанный компромисс: альтернатива (обрезка) уже давала
 * усечённый файл и ложный отчёт «готово», а лишний текст в файле модель
 * видит в результате вызова и может поправить. Разбор контента — в
 * parse_legacy_action: строки после CONTENT_END уже игнорируются там. */
size_t legacy_block_end(const std::string& text, size_t block_start) {
    size_t body_end = text.find("\n\n", block_start);
    if (body_end == std::string::npos) return text.size();
    const std::string cut = text.substr(block_start, body_end - block_start);
    if (cut.find("CONTENT_BEGIN") != std::string::npos &&
        cut.find("CONTENT_END") == std::string::npos) {
        const size_t end_marker = text.find("CONTENT_END", block_start);
        if (end_marker != std::string::npos) {
            const size_t line_end = text.find('\n', end_marker);
            return (line_end == std::string::npos) ? text.size() : line_end;
        }
        return text.size();
    }
    return body_end;
}

/* Ключи legacy-строк, по которым строка опознаётся как продолжение
 * блока вызова. Набор тот же, что разбирает parse_legacy_action
 * (TOOL/K/аргументы) плюс маркеры CONTENT. */
const char* const kLegacyKeys[] = {
    "PATH", "QUERY", "ROOT", "PATTERN",
    "CONTENT_BEGIN", "CONTENT", "CONTENT_END", "CLI", "URL", "K",
};

/* Первая непустая строка ПОСЛЕ line_end начинается с известного ключа. */
bool next_line_is_legacy_key(const std::string& text, size_t line_end) {
    size_t next = line_end + 1;
    while (next < text.size()) {
        const size_t eol = text.find('\n', next);
        const size_t nend = (eol == std::string::npos) ? text.size() : eol;
        size_t s = next;
        while (s < nend && (text[s] == ' ' || text[s] == '\t')) ++s;
        if (s < nend) {
            for (const char* k : kLegacyKeys) {
                const size_t kl = std::strlen(k);
                if (nend - s >= kl && text.compare(s, kl, k) == 0) return true;
            }
            return false;
        }
        if (eol == std::string::npos) break;
        next = eol + 1;
    }
    return false;
}

/* Строка bare-блока: «TOOL: <имя>» ровно с начала строки, имя —
 * идентификатор ASCII [A-Za-z0-9_-]+, строка им исчерпывается, а
 * СЛЕДУЮЩАЯ непустая строка начинается с известного ключа протокола.
 * Возвращает позицию строки «TOOL:» или npos.
 *
 * Зачем так строго. Bare-форма — ошибка модели (заголовок wp_action она
 * забыла), но проза в ответе никуда не делась, и слово «TOOL:» в начале
 * строки само по себе ничего не доказывает. Строгие условия выше отсекают
 * оба класса ложных срабатываний: русскую прозу («TOOL: список» — имя не
 * идентификатор) и одиночный заголовок без продолжения («TOOL: bash», а
 * дальше — проза, а не ключ протокола).
 *
 * Наблюдение, ради которого разбор заведён (qwen-flash, DashScope,
 * ses_000000000166, 2026-11): модель ответила РОВНО двумя строками
 * «TOOL: pip_install\nQUERY: tkinter» — без заголовка wp_action и без
 * фенса. extract_action блок не находил, весь текст становился текстом
 * хода, вызов не выполнялся; на напоминание модель повторяла тот же
 * bare-блок, сработал детектор застревания, задача закрылась принудительным
 * итогом. pip_install не выполнился ни разу, хотя разрешение было
 * получено, — tkinter так и не установился, и задача была провалена
 * из-за разбора, а не из-за возможностей модели. */
size_t find_bare_tool_line(const std::string& text) {
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t eol = text.find('\n', pos);
        const size_t line_end = (eol == std::string::npos) ? text.size() : eol;
        if (line_end - pos > 5 && text.compare(pos, 5, "TOOL:") == 0) {
            size_t name = pos + 5;
            while (name < line_end && (text[name] == ' ' || text[name] == '\t')) ++name;
            size_t name_end = name;
            while (name_end < line_end) {
                const char c = text[name_end];
                const bool id = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '_' || c == '-';
                if (!id) break;
                ++name_end;
            }
            size_t tail = name_end;
            while (tail < line_end &&
                   (text[tail] == ' ' || text[tail] == '\t' || text[tail] == '\r'))
                ++tail;
            if (name_end > name && tail == line_end &&
                next_line_is_legacy_key(text, line_end)) {
                return pos;
            }
        }
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return std::string::npos;
}

} // anonymous namespace

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
                    /* Граница блока — legacy_block_end (правило пустой
                     * строки и CONTENT_END — в комментарии к нему). */
                    const size_t end = legacy_block_end(text, body_start + 1);
                    std::string block = text.substr(body_start + 1, end - body_start - 1);
                    rest = text.substr(0, uf_wp) + text.substr(end);
                    return block;
                }
                /* Bare-блок «TOOL: …» вообще без заголовка wp_action.
                 * Тело блока — те же строки «KEY: value» от строки TOOL
                 * до границы (правило то же, см. legacy_block_end).
                 * Условия опознания — строгие (find_bare_tool_line),
                 * чтобы не утащить в вызов обычный текст ответа. */
                {
                    const size_t bare = find_bare_tool_line(text);
                    if (bare != std::string::npos) {
                        const size_t end = legacy_block_end(text, bare);
                        std::string block = text.substr(bare, end - bare);
                        rest = text.substr(0, bare) + text.substr(end);
                        return block;
                    }
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
    if (b == std::string::npos) {
        /* Огороженный wp_action БЕЗ закрывающего фенса: модель начала блок
         * и не закрыла его. Молчаливый отказ терял вызов ЦЕЛИКОМ — ход без
         * вызова читался циклом как «начало работы» и получал напоминание,
         * хотя модель свой вызов уже написала. Наблюдение на qwen-flash:
         * длинное содержимое write_file — модель «забывает» закрывающие
         * ``` (фенс с CONTENT режется сюда же по той же причине, что и
         * unfenced-блок выше). Блок — до конца текста; проверка содержимого
         * защищает обычный текстовый ответ, где открытый ``` без закрытия
         * — просто незакрытый пример кода. */
        const std::string head = text.substr(body + 1, 9);
        if (text.compare(a, 12, "```wp_action") == 0 || head == "wp_action") {
            std::string block = text.substr(body + 1);
            while (!block.empty() && (block.back() == '\n' || block.back() == '\r'))
                block.pop_back();
            rest = text.substr(0, a);
            return block;
        }
        return "";
    }
    std::string block = text.substr(body + 1, b - body - 1);
    rest = text.substr(0, a) + text.substr(b + 3);
    return block;
}

bool parse_legacy_action(const std::string& block, json::JsonValue& args) {
    args = json::JsonValue::object();
    std::istringstream iss(block);
    std::string line;
    bool in_content = false;
    while (std::getline(iss, line)) {
        while (!line.empty() && line.back() == '\r') line.pop_back();

        if (in_content) {
            if (line.find("CONTENT_END") != std::string::npos) { in_content = false; continue; }
            /* Строки CONTENT склеиваем обратно с переводами строк —
             * ровно так же, как это делал прежний разбор. */
            std::string cur = args.get_string("content");
            if (!cur.empty()) cur += '\n';
            cur += line;
            args.set("content", cur);
            continue;
        }
        if (line.find("CONTENT_BEGIN") != std::string::npos) { in_content = true; continue; }
        auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);
        trim_ws(key);
        trim_ws(val);
        if (key == "TOOL") args.set("tool", val);
        else if (key == "K") args.set("k", static_cast<long long>(std::atoi(val.c_str())));
        else {
            /* Имена legacy-ключей совпадают с ключами JSON-протокола
             * (PATH → path и т.д.), поэтому конвертируем только регистр. */
            for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            bool known = false;
            for (const auto& ak : g_action_keys) {
                if (key == ak.name) { known = true; break; }
            }
            if (known) args.set(key, val);        }
    }
    return !args.get_string("tool").empty();
}

bool parse_action(const std::string& block, json::JsonValue& args) {
    /* JSON-протокол: {"tool":"...","path":"...","k":N,...} (Фаза B3).
     *
     * Разбираем ПЕРВОЕ значение, а не весь блок: эвристики
     * extract_action вырезают блок «примерно», и за валидным объектом
     * может идти мусор (тот же случай, что и в json_parse_prefix). */
    size_t brace = block.find('{');
    if (brace != std::string::npos && block.find('}') != std::string::npos) {
        json::JsonValue parsed;
        size_t consumed = 0;
        if (json::JsonValue::parse_prefix(block, consumed, parsed) &&
            parsed.is_object() && !parsed.get_string("tool").empty()) {
            args = std::move(parsed);
            return true;
        }
        /* Если tool нет — это не наш JSON (обычный ответ), падаем в legacy. */
    }
    return parse_legacy_action(block, args);
}

json::JsonValue action_to_json(const Action& a) {
    json::JsonValue args = json::JsonValue::object();
    if (!a.tool.empty()) args.set("tool", a.tool);
    for (const auto& ak : g_action_keys) {
        const std::string& v = a.*ak.member;
        if (!v.empty()) args.set(ak.name, v);
    }
    if (a.k > 0) args.set("k", static_cast<long long>(a.k));
    return args;
}

void action_from_json(const json::JsonValue& args, Action& a) {
    a.tool = args.get_string("tool");
    for (const auto& ak : g_action_keys) {
        a.*ak.member = args.get_string(ak.name);
    }
    a.k = static_cast<int>(args.get_int("k", 0));
}

bool parse_action(const std::string& block, Action& a) {
    json::JsonValue args;
    if (!parse_action(block, args)) return false;
    action_from_json(args, a);
    return true;
}

void tool_args_from_json(const json::JsonValue& args, ToolArgs& a) {
    a.path = args.get_string("path");
    a.root = args.get_string("root");
    a.query = args.get_string("query");
    a.pattern = args.get_string("pattern");
    a.content = args.get_string("content");
    a.cli = args.get_string("cli");
    a.url = args.get_string("url");
    a.k = static_cast<int>(args.get_int("k", 0));
}

} // namespace coder