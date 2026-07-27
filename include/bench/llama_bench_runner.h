#pragma once

#include "bench_types.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>

namespace llama_gui {
namespace bench {

/**
 * @class LlamaBenchRunner
 * @brief Запуск llama-bench с заданными параметрами
 * 
 * Поддерживает:
 * - Асинхронный запуск
 * - Прогресс выполнения
 * - Перехват вывода
 * - Отмену выполнения
 * 
 * Пример использования:
 * @code
 * LlamaBenchRunner runner("/path/to/llama-bench");
 * runner.setProgressCallback([](int current, int total, const std::string& status) {
 *     std::cout << "Progress: " << current << "/" << total << " - " << status << std::endl;
 * });
 * runner.setOutputCallback([](const std::string& line) {
 *     std::cout << "Output: " << line << std::endl;
 * });
 * 
 * BenchTestParams params;
 * params.model_path = "/path/to/model.gguf";
 * params.n_prompt = 512;
 * params.n_gen = 128;
 * 
 * runner.startTest(params);
 * 
 * // Ждём завершения или проверяем статус
 * while (runner.isRunning()) {
 *     std::this_thread::sleep_for(std::chrono::milliseconds(100));
 * }
 * 
 * const auto& result = runner.getResult();
 * std::cout << "Prompt TPS: " << result.prompt_tokens_per_sec << std::endl;
 * @endcode
 */
class LlamaBenchRunner {
public:
    /**
     * @brief Колбэк прогресса
     * @param current Текущий тест (0-based)
     * @param total Всего тестов
     * @param status Строка статуса
     */
    using ProgressCallback = std::function<void(int current, int total, const std::string& status)>;
    
    /**
     * @brief Колбэк вывода (каждая строка из stdout/stderr)
     */
    using OutputCallback = std::function<void(const std::string& line)>;
    
    /**
     * @brief Колбэк завершения
     * @param result Результат выполнения
     */
    using CompletionCallback = std::function<void(const BenchRunResult&)>;

    /**
     * @brief Конструктор
     * @param llama_bench_path Путь к исполняемому файлу llama-bench
     */
    explicit LlamaBenchRunner(const std::string& llama_bench_path);
    
    /**
     * @brief Деструктор
     */
    ~LlamaBenchRunner();

    // Запрет копирования
    LlamaBenchRunner(const LlamaBenchRunner&) = delete;
    LlamaBenchRunner& operator=(const LlamaBenchRunner&) = delete;
    
    // Разрешение перемещения
    LlamaBenchRunner(LlamaBenchRunner&&) noexcept;
    LlamaBenchRunner& operator=(LlamaBenchRunner&&) noexcept;

    // =========================================================================
    // Запуск теста
    // =========================================================================
    
    /**
     * @brief Запустить тест (асинхронно)
     * @param params Параметры теста
     * @return true если запуск успешен
     */
    bool startTest(const BenchTestParams& params);
    
    /**
     * @brief Запустить тест с конкретным контекстом
     * @param params Параметры теста
     * @param context_depth Глубина контекста для этого запуска
     * @return true если запуск успешен
     */
    bool startTestWithContext(const BenchTestParams& params, int context_depth);

    /**
     * @brief Остановить текущий тест
     */
    void cancel();

    // =========================================================================
    // Статус
    // =========================================================================
    
    /**
     * @brief Проверить выполняется ли тест
     */
    bool isRunning() const;
    
    /**
     * @brief Получить текущий статус
     */
    BenchStatus getStatus() const;
    
    /**
     * @brief Получить прогресс (0-100%)
     */
    int getProgress() const;
    
    /**
     * @brief Получить текущий статус как строку
     */
    std::string getStatusString() const;

    // =========================================================================
    // Колбэки
    // =========================================================================
    
    /**
     * @brief Установить колбэк прогресса
     */
    void setProgressCallback(ProgressCallback cb);
    
    /**
     * @brief Установить колбэк вывода
     */
    void setOutputCallback(OutputCallback cb);
    
    /**
     * @brief Установить колбэк завершения
     */
    void setCompletionCallback(CompletionCallback cb);

    // =========================================================================
    // Результат
    // =========================================================================
    
    /**
     * @brief Получить результат после завершения
     */
    const BenchRunResult& getResult() const;

    // =========================================================================
    // Настройки
    // =========================================================================
    
    /**
     * @brief Установить формат вывода
     */
    void setOutputFormat(OutputFormat format);
    
    /**
     * @brief Установить режим подробного вывода
     */
    void setVerbose(bool verbose);
    
    /**
     * @brief Установить рабочую директорию
     */
    void setWorkingDirectory(const std::string& dir);
    
    /**
     * @brief Получить путь к llama-bench
     */
    std::string getLlamaBenchPath() const;

private:
    /**
     * @brief Внутренняя реализация (pimpl)
     */
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
    
    /**
     * @brief Запустить тест в отдельном потоке
     */
    void runTestAsync(const BenchTestParams& params, int context_depth = 0);
    
    /**
     * @brief Построить командную строку для llama-bench
     */
    std::string buildCommandLine(const BenchTestParams& params, int context_depth);
    
    /**
     * @brief Выполнить команду и получить вывод
     */
    int executeCommand(
        const std::string& command,
        std::string& output,
        std::string& error,
        bool capture_output = true
    );
    
    /**
     * @brief Распарсить вывод и создать результат
     */
    BenchRunResult parseOutput(const std::string& output, const std::string& error);
    
    /**
     * @brief Обновить прогресс и уведомить колбэк
     */
    void updateProgress(int current, int total, const std::string& status);
    
    /**
     * @brief Уведомить о выводе строки
     */
    void notifyOutput(const std::string& line);
    
    /**
     * @brief Уведомить о завершении
     */
    void notifyCompletion(const BenchRunResult& result);
};

} // namespace bench
} // namespace llama_gui
