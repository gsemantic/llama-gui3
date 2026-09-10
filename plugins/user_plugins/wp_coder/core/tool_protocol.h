#pragma once

/*
 * tool_protocol.h — Парсер протокола вызова инструментов AI-кодера (Фаза D2).
 *
 * Поддерживает два формата:
 *   1. Legacy: ```\nwp_action\nTOOL: имя\nPATH: ...\n```  (текстовый)
 *   2. JSON:   ```json\n{"tool":"имя","path":"...","k":5}\n```
 *
 * Разбор вынесен из engine.cpp в отдельный модуль, чтобы его можно было
 * тестировать изолированно и расширять, не трогая ReAct-цикл.
 */

#include <string>

namespace coder {

/* Вызов инструмента, распарсенный из ответа модели. */
struct Action {
    std::string tool, path, root, query, pattern, content, cli, url;
    int k = 6;
};

/* Извлечь блок вызова инструмента из текста ответа модели.
 * Возвращает блок (в legacy- или JSON-формате) и кладёт в rest всё остальное.
 * Если блока нет — возвращает пустую строку, rest = text. */
std::string extract_action(const std::string& text, std::string& rest);

/* Распарсить блок вызова (legacy wp_action ИЛИ JSON). Возвращает true, если
 * блок содержит валидный вызов инструмента (непустой tool). */
bool parse_action(const std::string& block, Action& a);

} // namespace coder