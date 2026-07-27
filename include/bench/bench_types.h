#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <chrono>
#include <map>

namespace llama_gui {
namespace bench {

/**
 * @brief Статус выполнения теста
 */
enum class BenchStatus {
    Pending,      // Ожидает запуска
    Running,      // Выполняется
    Completed,    // Успешно завершён
    Failed,       // Завершён с ошибкой
    Cancelled     // Отменён пользователем
};

/**
 * @brief Преобразовать статус в строку
 */
inline std::string statusToString(BenchStatus status) {
    switch (status) {
        case BenchStatus::Pending:    return "Pending";
        case BenchStatus::Running:    return "Running";
        case BenchStatus::Completed:  return "Completed";
        case BenchStatus::Failed:     return "Failed";
        case BenchStatus::Cancelled:  return "Cancelled";
        default:                      return "Unknown";
    }
}

/**
 * @brief Тип вывода llama-bench
 */
enum class OutputFormat {
    CSV,
    JSON,
    JSONL,
    Markdown,
    SQL
};

/**
 * @brief Преобразовать формат в строку (для командной строки)
 */
inline std::string formatToCliString(OutputFormat format) {
    switch (format) {
        case OutputFormat::CSV:      return "csv";
        case OutputFormat::JSON:     return "json";
        case OutputFormat::JSONL:    return "jsonl";
        case OutputFormat::Markdown: return "md";
        case OutputFormat::SQL:      return "sql";
        default:                     return "json";
    }
}

/**
 * @brief Параметры одного теста llama-bench
 *
 * Соответствуют опциям командной строки llama-bench:
 * -m, -p, -n, -d, -b, -ub, -t, -ngl, -ctk, -ctv, -r, --delay
 */
struct BenchTestParams {
    // Модель
    std::string model_path;
    std::string model_name;  // Короткое имя для отображения

    // Параметры теста
    int n_prompt = 256;        // количество токенов промпта (-p) - уменьшено для слабых машин
    int n_gen = 256;           // количество генерируемых токенов (-n) - уменьшено
    std::vector<int> n_depth;  // глубина контекста (-d), можно несколько значений

    // Параметры производительности
    int batch_size = 512;      // размер батча (-b) - уменьшено с 2048
    int ubatch_size = 256;     // микро-батч (-ub) - уменьшено с 512
    int threads = 2;           // потоки CPU (-t)
    int n_gpu_layers = 0;      // слои GPU (-ngl) - 0 вместо 99 (CPU-only по умолчанию)

    // Параметры кэша
    std::string cache_type_k = "f16";  // тип K кэша (-ctk) - f16 по умолчанию для совместимости
    std::string cache_type_v = "f16";  // тип V кэша (-ctv) - f16 по умолчанию для совместимости

    // Параметры выполнения
    int repetitions = 3;       // повторений (-r) - уменьшено с 5
    float delay_seconds = 0.0f;  // задержка между тестами (--delay)

    // Дополнительные опции
    bool flash_attn = false;   // Flash Attention (-fa)
    bool embeddings = false;   // режим embeddings (-embd)
    bool mmap = true;          // memory mapping (-mmp)
    int main_gpu = 0;          // основной GPU (-mg)
    int split_mode = 1;        // режим разделения (-sm): 0=none, 1=layer, 2=row

    // Конструктор по умолчанию
    BenchTestParams() = default;

    // Конструктор с инициализацией модели
    explicit BenchTestParams(const std::string& model)
        : model_path(model) {
        // Извлечь имя из пути
        size_t pos = model.find_last_of("/\\");
        model_name = (pos != std::string::npos) ? model.substr(pos + 1) : model;
    }
};

/**
 * @brief Результат одного запуска теста
 */
struct BenchRunResult {
    // Идентификаторы
    std::string test_id;
    std::string profile_name;      // Имя профиля (если использовался)
    std::string model_path;        // Путь к модели
    std::string model_name;        // Короткое имя модели
    
    // Статус
    BenchStatus status = BenchStatus::Pending;
    std::string error_message;
    
    // Параметры теста
    BenchTestParams params;
    
    // Основные метрики производительности
    double prompt_tokens_per_sec = 0.0;  // pp (tokens/sec)
    double gen_tokens_per_sec = 0.0;     // tg (tokens/sec)
    
    // Временные метрики
    double prompt_ms_total = 0.0;        // Общее время обработки промпта
    double gen_ms_total = 0.0;           // Общее время генерации
    double prompt_ms_per_token = 0.0;    // Время на токен промпта
    double gen_ms_per_token = 0.0;       // Время на токен генерации
    double ttft_ms = 0.0;                // Time To First Token
    
    // Статистика по повторениям
    int repetitions_completed = 0;
    std::vector<double> prompt_tpss;     // tokens/sec для каждого повторения
    std::vector<double> gen_tpss;
    
    // Использование памяти (если доступно)
    size_t gpu_memory_mb = 0;
    size_t cpu_memory_mb = 0;
    
    // Временные метки
    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point end_time;
    double duration_seconds = 0.0;
    
    // Контекст выполнения
    int context_depth = 0;  // Глубина контекста для этого конкретного запуска
    
    /**
     * @brief Проверить успешность выполнения
     */
    bool isSuccess() const {
        return status == BenchStatus::Completed && error_message.empty();
    }
    
    /**
     * @brief Получить строковое описание статуса
     */
    std::string getStatusString() const {
        return statusToString(status);
    }
};

/**
 * @brief Сводные результаты сравнения профилей
 */
struct BenchComparisonResult {
    // Идентификаторы
    std::string comparison_id;
    std::chrono::system_clock::time_point created_at;
    
    // Профили которые сравнивались
    std::vector<std::string> profile_names;
    std::vector<std::string> profile_paths;
    
    // Результаты по каждому профилю
    std::vector<BenchRunResult> results;
    
    // Лучший профиль по метрикам
    std::string best_prompt_tps_profile;
    std::string best_gen_tps_profile;
    std::string best_ttft_profile;
    
    // Статистика
    int total_tests_run = 0;
    int total_tests_failed = 0;
    double total_duration_seconds = 0.0;
    
    /**
     * @brief Получить результат по имени профиля
     */
    const BenchRunResult* getResultByProfileName(const std::string& name) const {
        for (const auto& result : results) {
            if (result.profile_name == name) {
                return &result;
            }
        }
        return nullptr;
    }
    
    /**
     * @brief Получить лучший результат по prompt TPS
     */
    const BenchRunResult* getBestPromptTpsResult() const {
        const BenchRunResult* best = nullptr;
        double max_tps = 0.0;
        
        for (const auto& result : results) {
            if (result.prompt_tokens_per_sec > max_tps) {
                max_tps = result.prompt_tokens_per_sec;
                best = &result;
            }
        }
        return best;
    }
    
    /**
     * @brief Получить лучший результат по generation TPS
     */
    const BenchRunResult* getBestGenTpsResult() const {
        const BenchRunResult* best = nullptr;
        double max_tps = 0.0;
        
        for (const auto& result : results) {
            if (result.gen_tokens_per_sec > max_tps) {
                max_tps = result.gen_tokens_per_sec;
                best = &result;
            }
        }
        return best;
    }
};

/**
 * @brief Прогресс выполнения бенчмарка
 */
struct BenchProgress {
    int current_test = 0;
    int total_tests = 0;
    int current_repetition = 0;
    int total_repetitions = 0;
    
    std::string current_profile;
    std::string current_status;
    
    double elapsed_seconds = 0.0;
    double estimated_remaining_seconds = 0.0;
    
    /**
     * @brief Получить процент выполнения
     */
    int getPercentComplete() const {
        if (total_tests == 0) return 0;
        return (current_test * 100) / total_tests;
    }
    
    /**
     * @brief Получить общее количество повторений
     */
    int getTotalRepetitionsDone() const {
        return current_test * total_repetitions + current_repetition;
    }
    
    /**
     * @brief Получить общее количество повторений
     */
    int getTotalRepetitions() const {
        return total_tests * total_repetitions;
    }
};

} // namespace bench
} // namespace llama_gui
