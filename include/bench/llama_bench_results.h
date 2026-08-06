#pragma once

#include "bench_types.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <chrono>

namespace llama_gui {
namespace bench {

/**
 * @class LlamaBenchResults
 * @brief Управление коллекцией результатов бенчмарков
 * 
 * Функции:
 * - Добавление результатов
 * - Сохранение/загрузка из файла
 * - Поиск и фильтрация
 * - Сравнение профилей
 * - Экспорт в различные форматы
 * 
 * Пример использования:
 * @code
 * LlamaBenchResults results;
 * 
 * // Добавить результат
 * BenchRunResult run_result;
 * run_result.model_path = "/path/to/model.gguf";
 * run_result.prompt_tokens_per_sec = 125.3;
 * run_result.gen_tokens_per_sec = 42.1;
 * results.addResult(run_result);
 * 
 * // Сохранить в файл
 * results.saveToFile("bench_results/results_20260325.json");
 * 
 * // Найти лучшие результаты
 * auto best = results.findBestByMetric("prompt_tokens_per_sec", 5);
 * 
 * // Экспорт в Markdown
 * std::string md = results.exportToMarkdown();
 * @endcode
 */
class LlamaBenchResults {
public:
    /**
     * @brief Конструктор по умолчанию
     */
    LlamaBenchResults();
    
    /**
     * @brief Конструктор с путём хранения
     * @param storage_path Путь к файлу/директории для хранения
     */
    explicit LlamaBenchResults(const std::string& storage_path);
    
    /**
     * @brief Деструктор
     */
    ~LlamaBenchResults();

    // =========================================================================
    // Добавление результатов
    // =========================================================================
    
    /**
     * @brief Добавить результат
     * @param result Результат для добавления
     */
    void addResult(const BenchRunResult& result);
    
    /**
     * @brief Добавить несколько результатов
     * @param results Вектор результатов
     */
    void addResults(const std::vector<BenchRunResult>& results);
    
    /**
     * @brief Добавить результат сравнения профилей
     * @param comparison Результат сравнения
     */
    void addComparison(const BenchComparisonResult& comparison);

    // =========================================================================
    // Сохранение и загрузка
    // =========================================================================
    
    /**
     * @brief Сохранить в файл JSON
     * @param file_path Путь к файлу
     * @return true если успешно
     */
    bool saveToFile(const std::string& file_path);
    
    /**
     * @brief Загрузить из файла JSON
     * @param file_path Путь к файлу
     * @return true если успешно
     */
    bool loadFromFile(const std::string& file_path);
    
    /**
     * @brief Сохранить в history.json
     * @return true если успешно
     */
    bool saveToHistory();
    
    /**
     * @brief Загрузить из history.json
     * @return true если успешно
     */
    bool loadFromHistory();

    // =========================================================================
    // Поиск и фильтрация
    // =========================================================================
    
    /**
     * @brief Найти результаты по имени профиля
     * @param profile_name Имя профиля
     * @return Вектор результатов
     */
    std::vector<BenchRunResult> findByProfile(const std::string& profile_name) const;
    
    /**
     * @brief Найти результаты по диапазону дат
     * @param from Начальная дата
     * @param to Конечная дата
     * @return Вектор результатов
     */
    std::vector<BenchRunResult> findByDateRange(
        std::chrono::system_clock::time_point from,
        std::chrono::system_clock::time_point to) const;
    
    /**
     * @brief Найти лучшие результаты по метрике
     * @param metric Название метрики ("prompt_tokens_per_sec", "gen_tokens_per_sec")
     * @param top_n Количество лучших результатов
     * @return Вектор результатов
     */
    std::vector<BenchRunResult> findBestByMetric(
        const std::string& metric, 
        int top_n = 5) const;
    
    /**
     * @brief Найти результаты по модели
     * @param model_path Путь к модели (или часть пути)
     * @return Вектор результатов
     */
    std::vector<BenchRunResult> findByModel(const std::string& model_path) const;
    
    /**
     * @brief Получить последний результат
     * @return Результат или nullptr если пусто
     */
    const BenchRunResult* getLastResult() const;

    // =========================================================================
    // Сравнение
    // =========================================================================
    
    /**
     * @brief Сравнить профили
     * @param profile_names Имена профилей для сравнения
     * @return Результат сравнения
     */
    BenchComparisonResult compareProfiles(
        const std::vector<std::string>& profile_names) const;
    
    /**
     * @brief Сравнить все доступные профили
     * @return Результат сравнения
     */
    BenchComparisonResult compareAllProfiles() const;

    // =========================================================================
    // Экспорт
    // =========================================================================
    
    /**
     * @brief Экспорт в JSON строку
     * @return JSON строка
     */
    std::string exportToJson() const;
    
    /**
     * @brief Экспорт в CSV строку
     * @return CSV строка
     */
    std::string exportToCsv() const;
    
    /**
     * @brief Экспорт в Markdown строку
     * @param include_comparison Включить таблицу сравнения
     * @return Markdown строка
     */
    std::string exportToMarkdown(bool include_comparison = true) const;
    
    /**
     * @brief Экспорт в файл
     * @param file_path Путь к файлу
     * @param format Формат файла
     * @return true если успешно
     */
    bool exportToFile(const std::string& file_path, OutputFormat format);

    // =========================================================================
    // Статистика
    // =========================================================================
    
    /**
     * @brief Получить общее количество результатов
     */
    size_t getTotalResults() const;
    
    /**
     * @brief Получить количество сравнений
     */
    size_t getComparisonsCount() const;
    
    /**
     * @brief Получить время последнего запуска
     */
    std::chrono::system_clock::time_point getLastRunTime() const;
    
    /**
     * @brief Получить список всех имён профилей
     */
    std::vector<std::string> getAllProfileNames() const;
    
    /**
     * @brief Получить список всех моделей
     */
    std::vector<std::string> getAllModelPaths() const;

    // =========================================================================
    // Очистка
    // =========================================================================
    
    /**
     * @brief Очистить все результаты
     */
    void clear();
    
    /**
     * @brief Удалить результаты старше указанной даты
     * @param cutoff Дата отсечки
     * @return Количество удалённых результатов
     */
    size_t removeOlderThan(std::chrono::system_clock::time_point cutoff);
    
    /**
     * @brief Удалить результат по ID
     * @param test_id ID теста
     * @return true если удалён
     */
    bool removeById(const std::string& test_id);

    // =========================================================================
    // Сериализация
    // =========================================================================
    
    /**
     * @brief Сериализовать в JSON объект nlohmann
     * @return JSON объект
     */
    nlohmann::json toJson() const;
    
    /**
     * @brief Загрузить из JSON объекта
     * @param json JSON объект
     */
    void loadFromJson(const nlohmann::json& json);

    // =========================================================================
    // Доступ к данным
    // =========================================================================
    
    /**
     * @brief Получить все результаты
     */
    const std::vector<BenchRunResult>& getResults() const;
    
    /**
     * @brief Получить все сравнения
     */
    const std::vector<BenchComparisonResult>& getComparisons() const;
    
    /**
     * @brief Получить путь хранения
     */
    std::string getStoragePath() const;
    
    /**
     * @brief Установить путь хранения
     */
    void setStoragePath(const std::string& path);

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
    std::string storage_path_;
    
    /**
     * @brief Инициализировать хранилище
     */
    void initializeStorage();
    
    /**
     * @brief Получить путь к history.json
     */
    std::string getHistoryFilePath() const;
    
    /**
     * @brief Обновить статистику сравнений
     */
    void updateComparisonStats(BenchComparisonResult& comparison) const;
};

} // namespace bench
} // namespace llama_gui
