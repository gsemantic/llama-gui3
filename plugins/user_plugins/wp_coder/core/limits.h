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
/* Минимальная длина текста, который цикл считает ИТОГОМ (И5.8).
 *
 * Четвёртое условие завершения, добавленное к трём портовым: у opencode
 * модели следуют протоколу finish, а здесь половина моделей — локальные
 * маленькие, и «Ок.» или «Готово, что дальше?» иначе закрывало бы задачу
 * пустым ответом. Порог не «качество текста», а признак «это ещё не
 * итог»: дальше цикл просит модель дать итог и продолжает ход. */
inline constexpr size_t kMinFinalAnswerLen = 300;

/* Универсальный предел вывода инструмента (И4.10, порт tool/truncate.ts).
 * Применяется к каждому инструменту в ToolsRegistry::run_output; остаток
 * пишется в файл, путь возвращается в ответе. */
inline constexpr size_t kMaxOutputLines = 2000;    // строк в RESULT
inline constexpr size_t kMaxOutputBytes = 50 * 1024;  // байт в RESULT

/* Компакшн (И7.1, порт session/overflow.ts): сколько токенов истории
 * оставляется под саму сводку, когда провайдер не задал резерв явно.
 * Живёт здесь, а не в core/compaction.h, по той же причине, что и
 * остальные лимиты: константы, разъехавшиеся по файлам, разъезжаются
 * и по значениям (шапка этого файла). */
inline constexpr long long kCompactionBuffer = 20000;

/* Сколько символов приходится на токен в оценке chars/4 (И7.2, порт
 * util/token.ts). Живёт рядом с буфером по той же причине: это число
 * решает, когда история начнёт считаться большой, и оно обязано быть
 * одно. */
inline constexpr long long kCharsPerToken = 4;

/* Результат инструмента в сводке (И7.4, порт
 * compaction.ts: TOOL_OUTPUT_MAX_CHARS). Сводка не должна раздуваться тем
 * же выводом, ради устранения которого её и затеяли. */
inline constexpr long long kSummaryToolOutputChars = 2000;

/* Сколько токенов ждём от сводки (И7.5, порт SUMMARY_OUTPUT_TOKENS).
 *
 * Ограничить генерацию числом мы не можем: блокирующий путь не передаёт
 * max_tokens, а текстовый протокол и подавно. Поэтому предел держится
 * двумя руками: промпт просит краткости (kCompactionSystemPrompt), а
 * эта константа — предохранитель, который обрезает слишком длинный
 * ответ по границе строки и помечает результат флагом
 * summary_truncated. Обрезка молча была бы хуже: сводка в 40 000 токенов
 * съела бы ровно то место, ради которого её составили. */
inline constexpr long long kCompactionSummaryMaxChars = 16000;

/* Границы хвоста, который остаётся при компакшне (И7.3, порт
 * compaction.ts: MIN/MAX_PRESERVE_RECENT_TOKENS). Снизу — чтобы у модели
 * осталось хоть что-то, сверху — чтобы сводка не вытеснила всё. */
inline constexpr long long kMinPreserveRecentTokens = 2000;
inline constexpr long long kMaxPreserveRecentTokens = 15000;

/* Прореживание вывода инструментов (И7.9, порт compaction.ts:
 * PRUNE_PROTECT, PRUNE_MINIMUM, PRUNE_PROTECTED_TOOLS).
 *
 * Что делает прореживание: полный вывод КАЖДОГО вызова инструмента живёт
 * в истории до конца сессии, и именно он, а не текст агента, съедает
 * окно. Старые результаты удаляются из контекста целиком (остаётся одна
 * строка-метка), и освобождённое место занимает хвост сжатия.
 *
 * kPruneProtectTokens — сколько ТОКЕНОВ вывода самых свежих вызовов
 * остаётся нетронутыми: их агент разбирает прямо сейчас, и очистить их
 * значило бы заставить его работать вслепую.
 *
 * kPruneMinimumTokens — снимать ли вообще. Ниже этой выгоды метка «очищено»
 * в промпте обходится дороже, чем сэкономленные токены: модель получит
 * несколько строк «[Old tool result content cleared]» вместо живого
 * текста и начнёт гадать, что случилось. Порог ниже порога защиты
 * намеренно: он отвечает на вопрос «а стоит ли», а тот — «что нельзя». */
inline constexpr long long kPruneProtectTokens = 40000;
inline constexpr long long kPruneMinimumTokens = 20000;

/* Инструменты, чей вывод НЕЛЬЗЯ прореживать, — тело навыка. Оно и есть
 * инструкция, и агент работает по ней прямо сейчас; очистить её значит
 * не освободить место, а сломать работу на середине. */
inline constexpr const char* kPruneProtectedTools[] = {"skill"};

/* Чем вывод очищенного вызова выглядит для модели (И7.9, порт
 * toolResult.ts: "[Old tool result content cleared]"). Строка на
 * английском — та же линия, что у меток формата и слова-выхода `compact`:
 * это протокол между плагином и моделью, взятый из порта дословно. Она
 * названа в kBaseSystemPrompt, иначе модель приняла бы её за содержимое
 * вывода и стала бы рассуждать о тексте, которого нет (ровно класс D2). */
inline constexpr const char* kClearedToolOutput = "[Old tool result content cleared]";

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