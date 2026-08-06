#pragma once

#include "bench_types.h"
#include "llama_bench_results.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace llama_gui {
namespace bench {

class LlamaBenchRunner;

/**
 * @class LlamaBenchModule
 * @brief Основная точка входа для функциональности llama-bench
 * 
 * Функции:
 * - Инициализация модуля
 * - Запуск бенчмарков
 * - Управление результатами
 * - Интеграция с UI
 * 
 * Пример использования:
 * @code
 * LlamaBenchModule bench_module;
 * 
 * // Инициализация
 * if (!bench_module.initialize("/path/to/llama-bench")) {
 *     std::cerr << "Failed to initialize" << std::endl;
 *     return;
 * }
 * 
 * // Настроить колбэки для UI
 * bench_module.setStatusCallback([](const std::string& status) {
 *     std::cout << "Status: " << status << std::endl;
 * });
 * 
 * bench_module.setProgressCallback([](int percent, const std::string& status) {
 *     std::cout << "Progress: " << percent << "% - " << status << std::endl;
 * });
 * 
 * // Запустить бенчмарк для одной модели
 * BenchTestParams params;
 * params.model_path = "/path/to/model.gguf";
 * params.n_prompt = 512;
 * params.n_gen = 128;
 * 
 * bench_module.runBenchmark("default", params);
 * 
 * // Запустить сравнение профилей
 * std::vector<std::string> profiles = {"default", "performance"};
 * bench_module.runComparison(profiles, params);
 * 
 * // Получить результаты
 * const auto& results = bench_module.getResults();
 * std::cout << "Total results: " << results.getTotalResults() << std::endl;
 * @endcode
 */
class LlamaBenchModule {
public:
    /**
     * @brief Колбэк статуса (строка статуса)
     */
    using StatusCallback = std::function<void(const std::string& status)>;
    
    /**
     * @brief Колбэк прогресса (процент, строка статуса)
     */
    using ProgressCallback = std::function<void(int percent, const std::string& status)>;
    
    /**
     * @brief Колбэк завершения бенчмарка (для перезапуска сервера)
     */
    using BenchmarkCompleteCallback = std::function<void()>;

    /**
     * @brief Конструктор
     */
    LlamaBenchModule();
    
    /**
     * @brief Деструктор
     */
    ~LlamaBenchModule();

    // =========================================================================
    // Инициализация
    // =========================================================================
    
    /**
     * @brief Инициализировать модуль
     * @param llama_bench_path Путь к исполняемому файлу llama-bench
     * @param results_directory Директория для хранения результатов
     * @return true если успешно
     */
    bool initialize(
        const std::string& llama_bench_path,
        const std::string& results_directory = "bench_results"
    );
    
    /**
     * @brief Завершить работу модуля
     */
    void shutdown();
    
    /**
     * @brief Проверить инициализирован ли модуль
     */
    bool isInitialized() const;

    // =========================================================================
    // Запуск бенчмарка
    // =========================================================================
    
    /**
     * @brief Запустить бенчмарк для профиля
     * @param profile_name Имя профиля (для идентификации)
     * @param params Параметры теста
     * @return true если запуск успешен
     */
    bool runBenchmark(const std::string& profile_name, const BenchTestParams& params);
    
    /**
     * @brief Запустить бенчмарк для модели
     * @param model_path Путь к модели
     * @param params Параметры теста
     * @return true если запуск успешен
     */
    bool runBenchmarkForModel(const std::string& model_path, const BenchTestParams& params);
    
    /**
     * @brief Запустить сравнение нескольких профилей
     * @param profile_paths Пути к профилям
     * @param params Параметры теста
     * @return true если запуск успешен
     */
    bool runComparison(
        const std::vector<std::string>& profile_paths,
        const BenchTestParams& params
    );
    
    /**
     * @brief Запустить бенчмарк с несколькими глубинами контекста
     * @param profile_name Имя профиля
     * @param params Параметры теста
     * @param depths Список глубин контекста
     * @return true если запуск успешен
     */
    bool runBenchmarkWithDepths(
        const std::string& profile_name,
        const BenchTestParams& params,
        const std::vector<int>& depths
    );

    // =========================================================================
    // Управление выполнением
    // =========================================================================
    
    /**
     * @brief Отменить текущий запуск
     */
    void cancelCurrentRun();
    
    /**
     * @brief Проверить выполняется ли бенчмарк
     */
    bool isRunning() const;
    
    /**
     * @brief Получить прогресс (0-100%)
     */
    int getProgress() const;
    
    /**
     * @brief Получить текущий статус как строку
     */
    std::string getCurrentStatus() const;
    
    /**
     * @brief Получить текущий профиль который тестируется
     */
    std::string getCurrentProfile() const;

    // =========================================================================
    // Результаты
    // =========================================================================
    
    /**
     * @brief Получить результаты (const версия)
     */
    const LlamaBenchResults& getResults() const;
    
    /**
     * @brief Получить результаты (не const версия)
     */
    LlamaBenchResults& getResults();
    
    /**
     * @brief Сохранить результаты в файл
     * @param file_path Путь к файлу
     * @return true если успешно
     */
    bool saveResults(const std::string& file_path);
    
    /**
     * @brief Загрузить результаты из файла
     * @param file_path Путь к файлу
     * @return true если успешно
     */
    bool loadResults(const std::string& file_path);

    // =========================================================================
    // Колбэки для UI
    // =========================================================================
    
    /**
     * @brief Установить колбэк статуса
     */
    void setStatusCallback(StatusCallback cb);
    
    /**
     * @brief Установить колбэк прогресса
     */
    void setProgressCallback(ProgressCallback cb);
    
    /**
     * @brief Установить колбэк завершения бенчмарка
     */
    void setBenchmarkCompleteCallback(BenchmarkCompleteCallback cb);

    // =========================================================================
    // Настройки
    // =========================================================================
    
    /**
     * @brief Установить параметры по умолчанию
     */
    void setDefaultParams(const BenchTestParams& params);
    
    /**
     * @brief Получить параметры по умолчанию
     */
    BenchTestParams getDefaultParams() const;
    
    /**
     * @brief Установить формат вывода
     */
    void setOutputFormat(OutputFormat format);
    
    /**
     * @brief Установить режим подробного вывода
     */
    void setVerbose(bool verbose);

    // =========================================================================
    // Информация
    // =========================================================================
    
    /**
     * @brief Получить путь к llama-bench
     */
    std::string getLlamaBenchPath() const;
    
    /**
     * @brief Получить директорию результатов
     */
    std::string getResultsDirectory() const;
    
    /**
     * @brief Получить версию модуля
     */
    static std::string getVersion();

private:
    /**
     * @brief Внутренняя реализация (pimpl)
     */
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
    
    /**
     * @brief Обработать завершение теста
     */
    void onTestComplete(const BenchRunResult& result);
    
    /**
     * @brief Обновить прогресс
     */
    void updateProgress(int percent, const std::string& status);
    
    /**
     * @brief Уведомить о статусе
     */
    void notifyStatus(const std::string& status);
    
    /**
     * @brief Запустить следующий профиль в сравнении
     */
    void runNextComparisonProfile();
};

} // namespace bench
} // namespace llama_gui
