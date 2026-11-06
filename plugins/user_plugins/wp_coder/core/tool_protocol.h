#pragma once

/*
 * tool_protocol.h — Парсер протокола вызова инструментов AI-кодера (Фаза D2).
 *
 * Поддерживает два формата:
 *   1. Legacy: ```\nwp_action\nTOOL: имя\nPATH: ...\n```  (текстовый)
 *   2. JSON:   ```json\n{"tool":"имя","path":"...","k":5}\n```
 *
 * И1.2: разбор JSON-аргументов переведён на JsonValue (core/json.h).
 * Раньше он дёргал json::str/json::int_, которые умеют только
 * верхнеуровневое строковое поле и молча возвращают 0 при любой
 * неудаче. Теперь аргументы — полноценный объект, поэтому добавление
 * нового параметра инструмента не требует правок здесь (D-3).
 *
 * extract_action НЕ ТРОНУТ: это 6 эвристик разбора ответа модели
 * (```wp_action, ```json, голый ``` с JSON, ```\nwp_action,
 * несфарендированный wp_action, несфарендированный JSON). Их поведение
 * закреплено 17 тестами парсера; ломать их переводом на DOM незачем —
 * эвристики работают на сырой строке по построению.
 */

#include "json.h"

#include <string>

namespace coder {

/* Вызов инструмента, распарсенный из ответа модели.
 *
 * И1.2: это тонкая проекция поверх JsonValue-аргументов, а не
 * самостоятельная структура. Существует для обратной совместимости
 * с существующими тестами и кодом; новый код работает с JsonValue
 * напрямую (см. parse_action → JsonValue). */
struct Action {
    std::string tool, path, root, query, pattern, content, cli, url;
    int k = 0;
};

/* Извлечь блок вызова инструмента из текста ответа модели.
 * Возвращает блок (в legacy- или JSON-формате) и кладёт в rest всё остальное.
 * Если блока нет — возвращает пустую строку, rest = text. */
std::string extract_action(const std::string& text, std::string& rest);

/* Разобрать блок в JsonValue-аргументы. Поддерживает оба формата.
 * Возвращает true, если блок содержит вызов инструмента (есть непустой
 * ключ "tool"). args в этом случае — объект с исходными ключами. */
bool parse_action(const std::string& block, json::JsonValue& args);

/* То же в старом представлении (9 фиксированных слотов). Реализовано
 * поверх parse_action(JsonValue&) — второй разбор не делается. */
bool parse_action(const std::string& block, Action& a);

/* Legacy-блок "TOOL: x\nPATH: y" → JsonValue-аргументы (те же ключи,
 * что и у JSON-протокола: tool, path, root, query, pattern, cli, url,
 * k, content). */
bool parse_legacy_action(const std::string& block, json::JsonValue& args);

/* Action ↔ JsonValue — для перехода старого кода на типизированные
 * аргументы по одному вызову, без миграции целиком. */
json::JsonValue action_to_json(const Action& a);
void action_from_json(const json::JsonValue& args, Action& a);

/* Обратная проекция в старые 8 слотов ToolArgs — нужна только
 * совместимому пути вызова инструмента (ToolsRegistry::run). */
void tool_args_from_json(const json::JsonValue& args, struct ToolArgs& a);

} // namespace coder
