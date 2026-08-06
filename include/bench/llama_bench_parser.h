#pragma once

#include "bench_types.h"
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace llama_gui {
namespace bench {

/**
 * @class LlamaBenchParser
 * @brief Парсинг вывода llama-bench в структурированные данные
 * 
 * Поддерживаемые форматы:
 * - JSON (основной)
 * - CSV
 * - Markdown таблицы
 * - JSONL (JSON Lines)
 * 
 * Пример использования:
 * @code
 * std::string json_output = "..."; // Вывод из llama-bench
 * auto results = LlamaBenchParser::parseJson(json_output);
 * 
 * for (const auto& result : results) {
 *     std::cout << "Model: " << result.model_path << std::endl;
 *     std::cout << "Prompt TPS: " << result.prompt_tokens_per_sec << std::endl;
 *     std::cout << "Gen TPS: " << result.gen_tokens_per_sec << std::endl;
 * }
 * @endcode
 */
class LlamaBenchParser {
public:
    // =========================================================================
    // Основные методы парсинга
    // =========================================================================
    
    /**
     * @brief Парсинг JSON вывода
     * @param json_input JSON строка от llama-bench
     * @return Вектор результатов
     */
    static std::vector<BenchRunResult> parseJson(const std::string& json_input);
    
    /**
     * @brief Парсинг CSV вывода
     * @param csv_input CSV строка
     * @return Вектор результатов
     */
    static std::vector<BenchRunResult> parseCsv(const std::string& csv_input);
    
    /**
     * @brief Парсинг Markdown таблицы
     * @param md_input Markdown строка с таблицей
     * @return Вектор результатов
     */
    static std::vector<BenchRunResult> parseMarkdown(const std::string& md_input);
    
    /**
     * @brief Парсинг JSONL (JSON Lines)
     * @param jsonl_input Строка с JSON объектами по одному на строку
     * @return Вектор результатов
     */
    static std::vector<BenchRunResult> parseJsonl(const std::string& jsonl_input);
    
    /**
     * @brief Авто-определение формата и парсинг
     * @param input Входные данные
     * @param hint Подсказка о формате (если известна)
     * @return Вектор результатов
     */
    static std::vector<BenchRunResult> parse(
        const std::string& input, 
        OutputFormat hint = OutputFormat::JSON
    );

    // =========================================================================
    // Валидация
    // =========================================================================
    
    /**
     * @brief Валидация результата
     * @param result Результат для проверки
     * @return true если результат корректен
     */
    static bool validateResult(const BenchRunResult& result);
    
    /**
     * @brief Проверка наличия всех обязательных метрик
     * @param result Результат
     * @return true если все метрики на месте
     */
    static bool hasAllMetrics(const BenchRunResult& result);

    // =========================================================================
    // Вспомогательные методы
    // =========================================================================
    
    /**
     * @brief Извлечь метрики из JSON объекта nlohmann
     * @param json_obj JSON объект
     * @return Результат бенчмарка
     */
    static BenchRunResult extractFromJson(const nlohmann::json& json_obj);
    
    /**
     * @brief Создать результат из CSV строки
     * @param csv_line Одна строка CSV
     * @param headers Заголовки колонок
     * @return Результат бенчмарка
     */
    static BenchRunResult extractFromCsv(
        const std::string& csv_line, 
        const std::vector<std::string>& headers
    );
    
    /**
     * @brief Определить формат ввода по содержимому
     * @param input Входные данные
     * @return Определённый формат
     */
    static OutputFormat detectFormat(const std::string& input);

private:
    // =========================================================================
    // Внутренние вспомогательные методы
    // =========================================================================
    
    /**
     * @brief Разделить CSV строку на поля
     */
    static std::vector<std::string> splitCsvLine(const std::string& line);
    
    /**
     * @brief Разделить Markdown таблицу на строки
     */
    static std::vector<std::string> splitMarkdownTable(const std::string& md);
    
    /**
     * @brief Разделить строку по разделителю
     */
    static std::vector<std::string> splitString(
        const std::string& str, 
        char delimiter
    );
    
    /**
     * @brief Удалить пробелы по краям строки
     */
    static std::string trim(const std::string& str);
    
    /**
     * @brief Преобразовать строку в double
     */
    static double parseDouble(const std::string& str, double default_val = 0.0);
    
    /**
     * @brief Преобразовать строку в int
     */
    static int parseInt(const std::string& str, int default_val = 0);
    
    /**
     * @brief Преобразовать строку в статус
     */
    static BenchStatus parseStatus(const std::string& str);
    
    /**
     * @brief Извлечь значение из Markdown ячейки
     */
    static std::string extractMarkdownCell(const std::string& cell);
    
    /**
     * @brief Найти заголовки в Markdown таблице
     */
    static std::vector<std::string> parseMarkdownHeaders(
        const std::vector<std::string>& lines
    );
    
    /**
     * @brief Найти разделитель в Markdown таблице
     */
    static int findMarkdownSeparator(const std::vector<std::string>& lines);
};

} // namespace bench
} // namespace llama_gui
