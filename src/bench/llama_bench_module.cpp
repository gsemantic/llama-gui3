#include "../../include/bench/llama_bench_module.h"
#include "../../include/bench/llama_bench_runner.h"
#include "../../include/bench/llama_bench_parser.h"
#include "../../include/bench/bench_common.h"
#include "../../include/bench/profile_adapter.h"

#include <mutex>
#include <atomic>
#include <stdexcept>
#include <iostream>

namespace llama_gui {
namespace bench {

/**
 * @brief Внутренняя реализация LlamaBenchModule
 */
struct LlamaBenchModule::Impl {
    std::string llama_bench_path;
    std::string results_directory;
    
    std::unique_ptr<LlamaBenchRunner> runner;
    LlamaBenchResults results;
    
    std::atomic<bool> initialized{false};
    std::atomic<bool> running{false};
    std::atomic<int> progress{0};
    
    std::string current_status;
    std::string current_profile;
    
    BenchTestParams default_params;
    OutputFormat output_format = OutputFormat::JSON;
    bool verbose = false;
    
    StatusCallback status_callback;
    ProgressCallback progress_callback;
    BenchmarkCompleteCallback benchmark_complete_callback;
    
    mutable std::mutex mutex;
    
    // Для сравнения профилей
    std::vector<std::string> comparison_profiles;
    size_t current_comparison_index = 0;
    BenchComparisonResult current_comparison;
};

// ============================================================================
// Конструктор/деструктор
// ============================================================================

LlamaBenchModule::LlamaBenchModule()
    : pimpl_(std::make_unique<Impl>())
{
}

LlamaBenchModule::~LlamaBenchModule() {
    if (isRunning()) {
        cancelCurrentRun();
    }
    shutdown();
}

// ============================================================================
// Инициализация
// ============================================================================

bool LlamaBenchModule::initialize(
    const std::string& llama_bench_path,
    const std::string& results_directory)
{
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    if (pimpl_->initialized) {
        return true;  // Уже инициализирован
    }
    
    // Проверить существование llama-bench
    if (!BenchCommon::fileExists(llama_bench_path)) {
        pimpl_->current_status = "llama-bench not found: " + llama_bench_path;
        return false;
    }
    
    pimpl_->llama_bench_path = llama_bench_path;
    pimpl_->results_directory = results_directory;

    // Создать директорию для результатов
    BenchCommon::createDirectory(results_directory);

    // Инициализировать результаты (используем конструктор по умолчанию)
    // pimpl_->results уже инициализирован в конструкторе LlamaBenchResults
    
    // Создать runner
    try {
        pimpl_->runner = std::make_unique<LlamaBenchRunner>(llama_bench_path);
        
        // Настроить колбэки runner
        pimpl_->runner->setCompletionCallback([this](const BenchRunResult& result) {
            onTestComplete(result);
        });
        
        pimpl_->runner->setProgressCallback([this](int current, int total, const std::string& status) {
            int percent = (total > 0) ? (current * 100 / total) : 0;
            updateProgress(percent, status);
        });
        
        pimpl_->runner->setOutputCallback([this](const std::string& line) {
            if (pimpl_->verbose) {
                notifyStatus("Output: " + line);
            }
        });
        
    } catch (const std::exception& e) {
        pimpl_->current_status = std::string("Failed to create runner: ") + e.what();
        return false;
    }
    
    pimpl_->initialized = true;
    pimpl_->current_status = "Initialized";
    
    return true;
}

void LlamaBenchModule::shutdown() {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    if (isRunning()) {
        cancelCurrentRun();
    }
    
    // Сохранить результаты
    pimpl_->results.saveToHistory();
    
    pimpl_->runner.reset();
    pimpl_->initialized = false;
    pimpl_->current_status = "Shutdown";
}

bool LlamaBenchModule::isInitialized() const {
    return pimpl_->initialized;
}

// ============================================================================
// Запуск бенчмарка
// ============================================================================

bool LlamaBenchModule::runBenchmark(const std::string& profile_name, const BenchTestParams& params) {
    if (!pimpl_->initialized || !pimpl_->runner) {
        return false;
    }

    if (isRunning()) {
        return false;  // Уже выполняется
    }

    std::lock_guard<std::mutex> lock(pimpl_->mutex);

    pimpl_->current_profile = profile_name;
    pimpl_->running = true;
    pimpl_->progress = 0;

    notifyStatus("Starting benchmark for: " + profile_name);

    // Запустить тест
    BenchTestParams test_params = params;
    if (test_params.model_name.empty()) {
        test_params.model_name = profile_name;
    }
    
    // Ограничить параметры для быстрого бенчмарка
    // На слабых CPU большие значения приводят к таймауту
    if (test_params.n_prompt > 64) {
        std::cout << "[LlamaBenchModule] Limiting n_prompt from " << test_params.n_prompt 
                  << " to 64 for faster benchmark" << std::endl;
        test_params.n_prompt = 64;
    }
    if (test_params.n_gen > 32) {
        std::cout << "[LlamaBenchModule] Limiting n_gen from " << test_params.n_gen 
                  << " to 32 for faster benchmark" << std::endl;
        test_params.n_gen = 32;
    }

    return pimpl_->runner->startTest(test_params);
}

bool LlamaBenchModule::runBenchmarkForModel(const std::string& model_path, const BenchTestParams& params) {
    std::string profile_name = BenchCommon::extractFileNameWithoutExt(model_path);
    
    BenchTestParams test_params = params;
    test_params.model_path = model_path;
    test_params.model_name = profile_name;
    
    return runBenchmark(profile_name, test_params);
}

bool LlamaBenchModule::runComparison(
    const std::vector<std::string>& profile_paths,
    const BenchTestParams& params)
{
    if (!pimpl_->initialized || !pimpl_->runner) {
        return false;
    }
    
    if (isRunning()) {
        return false;  // Уже выполняется
    }
    
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    if (profile_paths.empty()) {
        return false;
    }
    
    // Инициализировать сравнение
    pimpl_->comparison_profiles = profile_paths;
    pimpl_->current_comparison_index = 0;
    pimpl_->current_comparison = BenchComparisonResult();
    pimpl_->current_comparison.comparison_id = BenchCommon::getTimestampForFileName();
    pimpl_->current_comparison.created_at = std::chrono::system_clock::now();
    pimpl_->current_comparison.profile_paths = profile_paths;
    
    for (const auto& path : profile_paths) {
        pimpl_->current_comparison.profile_names.push_back(
            BenchCommon::extractFileNameWithoutExt(path)
        );
    }
    
    pimpl_->running = true;
    pimpl_->progress = 0;
    
    notifyStatus("Starting comparison of " + std::to_string(profile_paths.size()) + " profiles");
    
    // Запустить первый профиль
    runNextComparisonProfile();
    
    return true;
}

bool LlamaBenchModule::runBenchmarkWithDepths(
    const std::string& profile_name,
    const BenchTestParams& params,
    const std::vector<int>& depths)
{
    if (!pimpl_->initialized || !pimpl_->runner) {
        return false;
    }
    
    if (depths.empty()) {
        // Если глубин нет, запустить обычный бенчмарк
        return runBenchmark(profile_name, params);
    }
    
    // Запустить бенчмарк для каждой глубины
    // Это упрощённая реализация - в полной версии нужно запускать последовательно
    
    notifyStatus("Benchmark with " + std::to_string(depths.size()) + " depth levels");
    
    // Для простоты, запустить с первой глубиной
    BenchTestParams test_params = params;
    test_params.n_depth = depths;
    
    return runBenchmark(profile_name, test_params);
}

// ============================================================================
// Управление выполнением
// ============================================================================

void LlamaBenchModule::cancelCurrentRun() {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    if (pimpl_->runner && isRunning()) {
        pimpl_->runner->cancel();
    }
    
    pimpl_->running = false;
    pimpl_->current_status = "Cancelled";
    
    notifyStatus("Benchmark cancelled");
}

bool LlamaBenchModule::isRunning() const {
    return pimpl_->running;
}

int LlamaBenchModule::getProgress() const {
    return pimpl_->progress;
}

std::string LlamaBenchModule::getCurrentStatus() const {
    return pimpl_->current_status;
}

std::string LlamaBenchModule::getCurrentProfile() const {
    return pimpl_->current_profile;
}

// ============================================================================
// Результаты
// ============================================================================

const LlamaBenchResults& LlamaBenchModule::getResults() const {
    return pimpl_->results;
}

LlamaBenchResults& LlamaBenchModule::getResults() {
    return pimpl_->results;
}

bool LlamaBenchModule::saveResults(const std::string& file_path) {
    return pimpl_->results.saveToFile(file_path);
}

bool LlamaBenchModule::loadResults(const std::string& file_path) {
    return pimpl_->results.loadFromFile(file_path);
}

// ============================================================================
// Колбэки
// ============================================================================

void LlamaBenchModule::setStatusCallback(StatusCallback cb) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    pimpl_->status_callback = std::move(cb);
}

void LlamaBenchModule::setProgressCallback(ProgressCallback cb) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    pimpl_->progress_callback = std::move(cb);
}

void LlamaBenchModule::setBenchmarkCompleteCallback(BenchmarkCompleteCallback cb) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    pimpl_->benchmark_complete_callback = std::move(cb);
}

// ============================================================================
// Настройки
// ============================================================================

void LlamaBenchModule::setDefaultParams(const BenchTestParams& params) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    pimpl_->default_params = params;
}

BenchTestParams LlamaBenchModule::getDefaultParams() const {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    return pimpl_->default_params;
}

void LlamaBenchModule::setOutputFormat(OutputFormat format) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    pimpl_->output_format = format;
    
    if (pimpl_->runner) {
        pimpl_->runner->setOutputFormat(format);
    }
}

void LlamaBenchModule::setVerbose(bool verbose) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    pimpl_->verbose = verbose;
    
    if (pimpl_->runner) {
        pimpl_->runner->setVerbose(verbose);
    }
}

// ============================================================================
// Информация
// ============================================================================

std::string LlamaBenchModule::getLlamaBenchPath() const {
    return pimpl_->llama_bench_path;
}

std::string LlamaBenchModule::getResultsDirectory() const {
    return pimpl_->results_directory;
}

std::string LlamaBenchModule::getVersion() {
    return "1.0.0";
}

// ============================================================================
// Приватные методы
// ============================================================================

void LlamaBenchModule::onTestComplete(const BenchRunResult& result) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    // Добавить результат
    pimpl_->results.addResult(result);
    
    // Логирование для отладки
    std::cout << "Benchmark complete: " << result.profile_name << std::endl;
    std::cout << "  Status: " << result.getStatusString() << std::endl;
    std::cout << "  Model: " << result.model_name << std::endl;
    std::cout << "  Prompt TPS: " << result.prompt_tokens_per_sec << std::endl;
    std::cout << "  Gen TPS: " << result.gen_tokens_per_sec << std::endl;
    if (!result.error_message.empty()) {
        std::cout << "  Error: " << result.error_message << std::endl;
    }
    
    // Обновить статус
    if (result.isSuccess()) {
        pimpl_->current_status = "Completed: " + result.profile_name;
        notifyStatus("Benchmark completed: " + result.profile_name + 
                    " (Prompt: " + BenchCommon::formatSpeed(result.prompt_tokens_per_sec) + " t/s, " +
                    "Gen: " + BenchCommon::formatSpeed(result.gen_tokens_per_sec) + " t/s)");
    } else {
        pimpl_->current_status = "Failed: " + result.profile_name;
        notifyStatus("Benchmark failed: " + result.profile_name + " - " + result.error_message);
    }
    
    // Если это часть сравнения, запустить следующий профиль
    if (!pimpl_->comparison_profiles.empty() &&
        pimpl_->current_comparison_index < pimpl_->comparison_profiles.size() - 1) {
        pimpl_->current_comparison.results.push_back(result);
        runNextComparisonProfile();
    } else {
        // Завершение сравнения или одиночного теста
        pimpl_->running = false;

        if (!pimpl_->comparison_profiles.empty()) {
            pimpl_->current_comparison.results.push_back(result);
            pimpl_->results.addComparison(pimpl_->current_comparison);

            notifyStatus("Comparison completed: " + pimpl_->current_comparison.comparison_id);

            // Очистить сравнение
            pimpl_->comparison_profiles.clear();
            pimpl_->current_comparison_index = 0;
        }
        
        // Уведомить о полном завершении (для перезапуска сервера)
        if (pimpl_->benchmark_complete_callback) {
            pimpl_->benchmark_complete_callback();
        }
    }
}

void LlamaBenchModule::updateProgress(int percent, const std::string& status) {
    pimpl_->progress = percent;
    pimpl_->current_status = status;
    
    if (pimpl_->progress_callback) {
        pimpl_->progress_callback(percent, status);
    }
}

void LlamaBenchModule::notifyStatus(const std::string& status) {
    if (pimpl_->status_callback) {
        pimpl_->status_callback(status);
    }
}

void LlamaBenchModule::runNextComparisonProfile() {
    // Этот метод вызывается внутри lock

    if (pimpl_->current_comparison_index >= pimpl_->comparison_profiles.size()) {
        return;
    }

    std::string profile_path = pimpl_->comparison_profiles[pimpl_->current_comparison_index];
    std::string profile_name = BenchCommon::extractFileNameWithoutExt(profile_path);

    notifyStatus("Running comparison " +
                 std::to_string(pimpl_->current_comparison_index + 1) + "/" +
                 std::to_string(pimpl_->comparison_profiles.size()) +
                 ": " + profile_name);

    // Загрузить параметры из JSON профиля
    BenchTestParams params = ProfileAdapter::profileToBenchParams(profile_path);

    // Переопределить параметрами по умолчанию если они заданы
    if (pimpl_->default_params.n_prompt > 0) {
        params.n_prompt = pimpl_->default_params.n_prompt;
    }
    if (pimpl_->default_params.n_gen > 0) {
        params.n_gen = pimpl_->default_params.n_gen;
    }
    
    // Ограничить количество токенов генерации для бенчмарка (максимум 256)
    // Это предотвращает слишком долгие тесты
    if (params.n_gen > 256) {
        std::cout << "[LlamaBenchModule] Limiting n_gen from " << params.n_gen 
                  << " to 256 for faster benchmark" << std::endl;
        params.n_gen = 256;
    }
    
    if (pimpl_->default_params.batch_size > 0) {
        params.batch_size = pimpl_->default_params.batch_size;
    }
    if (pimpl_->default_params.threads > 0) {
        params.threads = pimpl_->default_params.threads;
    }
    if (pimpl_->default_params.n_gpu_layers >= 0) {
        params.n_gpu_layers = pimpl_->default_params.n_gpu_layers;
    }
    if (pimpl_->default_params.repetitions > 0) {
        params.repetitions = pimpl_->default_params.repetitions;
    }
    if (!pimpl_->default_params.n_depth.empty()) {
        params.n_depth = pimpl_->default_params.n_depth;
    }
    
    // Проверить что модель найдена
    if (params.model_path.empty()) {
        std::cerr << "Error: Model path not found in profile: " << profile_path << std::endl;
        
        // Создать失败 результат
        BenchRunResult failed_result;
        failed_result.profile_name = profile_name;
        failed_result.model_path = profile_path;
        failed_result.status = BenchStatus::Failed;
        failed_result.error_message = "Model path not found in profile";
        onTestComplete(failed_result);
        
        pimpl_->current_comparison_index++;
        return;
    }

    pimpl_->current_profile = profile_name;

    // Освободить lock перед запуском (чтобы избежать deadlock в колбэках)
    pimpl_->mutex.unlock();

    bool started = pimpl_->runner->startTest(params);

    pimpl_->mutex.lock();  // Вернуть lock

    if (started) {
        pimpl_->current_comparison_index++;
    }
}

} // namespace bench
} // namespace llama_gui
