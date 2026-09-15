#pragma once

/*
 * limits.h — единые константы лимитов AI-кодера (Фаза 4.5).
 *
 * Все лимиты жили в каждом файле отдельно (kMaxToolOutput, kWpMaxOutput,
 * kSessionBudget и т.д.) и расходились. Теперь — единственный источник.
 */

#include <cstddef>

namespace coder {
namespace limits {

/* Вывод инструментов: результат попадает в следующий запрос к LLM,
 * поэтому его размер напрямую определяет скорость и стоимость префилля. */
inline constexpr size_t kMaxToolOutput = 12000;   // байт на результат инструмента
inline constexpr size_t kModuleMaxOutput = 8000;  // python/wp инструменты
inline constexpr size_t kMaxGrepMatches = 200;    // строк совпадений
inline constexpr size_t kReadFileChars = 12000;   // байт на read_file
inline constexpr size_t kMaxSymFile = 40;         // символов в файле для repo_map

/* ReAct-цикл. */
inline constexpr int kMaxSteps = 12;              // шагов на задачу (было 8 vs 12 — исправлено)
inline constexpr size_t kSessionBudget = 60000;   // бюджет символов истории сессии
inline constexpr size_t kResultBudget = 1500;     // обрезка одного RESULT в сессии

} // namespace limits
} // namespace coder