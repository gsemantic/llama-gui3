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

/* Универсальный предел вывода инструмента (И4.10, порт tool/truncate.ts).
 * Применяется к каждому инструменту в ToolsRegistry::run_output; остаток
 * пишется в файл, путь возвращается в ответе. */
inline constexpr size_t kMaxOutputLines = 2000;    // строк в RESULT
inline constexpr size_t kMaxOutputBytes = 50 * 1024;  // байт в RESULT

/* shell (И4.8). 120 с — как в opencode; раньше было 60, и `docker build`
 * на нормальном проекте не успевал. */
inline constexpr unsigned kShellTimeoutSec = 120;

/* Чтение файла (И4.7). */
inline constexpr size_t kReadDefaultLines = 2000;  // строк по умолчанию
inline constexpr size_t kBinarySniffBytes = 8000;  // сколько байт нюхаем на бинарность
inline constexpr double kBinaryNonPrintableRatio = 0.3;  // доля непечатаемых = «двоичный»

/* list (И4.9): записей в ответе. */
inline constexpr size_t kListLimit = 300;

/* glob (И4.1). Лимит результата задан планом; kGlobMaxVisited —
 * предохранитель обхода, без него «*.zip в /» сканировал бы диск. */
inline constexpr size_t kGlobLimit = 100;         // файлов в ответе glob
inline constexpr size_t kGlobMaxVisited = 20000;  // записей каталога за один обход

} // namespace limits
} // namespace coder