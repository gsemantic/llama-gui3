#include "../../include/bench/llama_bench_runner.h"
#include "../../include/bench/bench_common.h"

#include <array>
#include <cstring>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <csignal>
#include <poll.h>
#include <fcntl.h>
#include <chrono>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/select.h>

namespace llama_gui {
namespace bench {

/**
 * @brief Внутренняя реализация LlamaBenchRunner
 */
struct LlamaBenchRunner::Impl {
    std::string llama_bench_path;
    std::string working_directory;
    
    std::atomic<BenchStatus> status{BenchStatus::Pending};
    std::atomic<int> progress{0};
    std::atomic<bool> running{false};
    
    BenchTestParams current_params;
    int current_context_depth = 0;
    
    BenchRunResult result;
    
    ProgressCallback progress_callback;
    OutputCallback output_callback;
    CompletionCallback completion_callback;
    
    OutputFormat output_format = OutputFormat::JSON;
    bool verbose = false;
    
    std::thread worker_thread;
    mutable std::mutex mutex;
    
    int process_id = -1;
    bool cancelled = false;
};

// ============================================================================
// Конструктор/деструктор
// ============================================================================

LlamaBenchRunner::LlamaBenchRunner(const std::string& llama_bench_path)
    : pimpl_(std::make_unique<Impl>()) 
{
    pimpl_->llama_bench_path = llama_bench_path;
    
    // Проверить существование файла
    if (!BenchCommon::fileExists(llama_bench_path)) {
        throw std::runtime_error("llama-bench not found at: " + llama_bench_path);
    }
}

LlamaBenchRunner::~LlamaBenchRunner() {
    if (isRunning()) {
        cancel();
    }
}

LlamaBenchRunner::LlamaBenchRunner(LlamaBenchRunner&&) noexcept = default;
LlamaBenchRunner& LlamaBenchRunner::operator=(LlamaBenchRunner&&) noexcept = default;

// ============================================================================
// Запуск теста
// ============================================================================

bool LlamaBenchRunner::startTest(const BenchTestParams& params) {
    return startTestWithContext(params, 0);
}

bool LlamaBenchRunner::startTestWithContext(const BenchTestParams& params, int context_depth) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    if (pimpl_->running) {
        return false;  // Уже выполняется
    }
    
    pimpl_->current_params = params;
    pimpl_->current_context_depth = context_depth;
    pimpl_->status = BenchStatus::Running;
    pimpl_->running = true;
    pimpl_->progress = 0;
    pimpl_->cancelled = false;
    
    // Инициализировать результат
    pimpl_->result = BenchRunResult();
    pimpl_->result.test_id = BenchCommon::getTimestampForFileName();
    pimpl_->result.profile_name = params.model_name;
    pimpl_->result.model_path = params.model_path;
    pimpl_->result.status = BenchStatus::Running;
    pimpl_->result.params = params;
    pimpl_->result.context_depth = context_depth;
    pimpl_->result.start_time = std::chrono::system_clock::now();
    
    // Запустить в отдельном потоке
    runTestAsync(params, context_depth);
    
    return true;
}

void LlamaBenchRunner::cancel() {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    if (!pimpl_->running) {
        return;
    }
    
    pimpl_->cancelled = true;
    pimpl_->status = BenchStatus::Cancelled;
    
    // Убить процесс если возможно
    if (pimpl_->process_id > 0) {
        // POSIX: kill процесс
        ::kill(pimpl_->process_id, SIGTERM);
    }
    
    pimpl_->running = false;
    
    // Уведомить о завершении
    pimpl_->result.status = BenchStatus::Cancelled;
    pimpl_->result.end_time = std::chrono::system_clock::now();
    pimpl_->result.error_message = "Cancelled by user";
    
    notifyCompletion(pimpl_->result);
}

// ============================================================================
// Статус
// ============================================================================

bool LlamaBenchRunner::isRunning() const {
    return pimpl_->running;
}

BenchStatus LlamaBenchRunner::getStatus() const {
    return pimpl_->status;
}

int LlamaBenchRunner::getProgress() const {
    return pimpl_->progress;
}

std::string LlamaBenchRunner::getStatusString() const {
    return statusToString(pimpl_->status.load());
}

// ============================================================================
// Колбэки
// ============================================================================

void LlamaBenchRunner::setProgressCallback(ProgressCallback cb) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    pimpl_->progress_callback = std::move(cb);
}

void LlamaBenchRunner::setOutputCallback(OutputCallback cb) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    pimpl_->output_callback = std::move(cb);
}

void LlamaBenchRunner::setCompletionCallback(CompletionCallback cb) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    pimpl_->completion_callback = std::move(cb);
}

// ============================================================================
// Результат
// ============================================================================

const BenchRunResult& LlamaBenchRunner::getResult() const {
    return pimpl_->result;
}

// ============================================================================
// Настройки
// ============================================================================

void LlamaBenchRunner::setOutputFormat(OutputFormat format) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    pimpl_->output_format = format;
}

void LlamaBenchRunner::setVerbose(bool verbose) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    pimpl_->verbose = verbose;
}

void LlamaBenchRunner::setWorkingDirectory(const std::string& dir) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    pimpl_->working_directory = dir;
}

std::string LlamaBenchRunner::getLlamaBenchPath() const {
    return pimpl_->llama_bench_path;
}

// ============================================================================
// Приватные методы
// ============================================================================

void LlamaBenchRunner::runTestAsync(const BenchTestParams& params, int context_depth) {
    pimpl_->worker_thread = std::thread([this, params, context_depth]() {
        try {
            // Построить командную строку
            std::string cmd = buildCommandLine(params, context_depth);

            // Всегда выводить команду для отладки
            std::cout << "LlamaBench: Command: " << cmd << std::endl;
            
            if (pimpl_->verbose) {
                notifyOutput("Command: " + cmd);
            }

            // Выполнить команду
            std::string output;
            std::string error;

            updateProgress(0, params.repetitions, "Starting...");

            int exit_code = executeCommand(cmd, output, error);
            
            // Проверить отмену
            if (pimpl_->cancelled) {
                return;
            }
            
            // Распарсить результат
            pimpl_->result = parseOutput(output, error);
            
            // Установить статус
            if (exit_code == 0 && pimpl_->result.status != BenchStatus::Failed) {
                pimpl_->result.status = BenchStatus::Completed;
                pimpl_->status = BenchStatus::Completed;
            } else {
                pimpl_->result.status = BenchStatus::Failed;
                pimpl_->status = BenchStatus::Failed;
                
                if (error.empty() && pimpl_->result.error_message.empty()) {
                    pimpl_->result.error_message = "Exit code: " + std::to_string(exit_code);
                } else if (!error.empty()) {
                    pimpl_->result.error_message = error;
                }
            }
            
            // Завершить
            pimpl_->result.end_time = std::chrono::system_clock::now();
            auto duration = pimpl_->result.end_time - pimpl_->result.start_time;
            pimpl_->result.duration_seconds = std::chrono::duration<double>(duration).count();
            
            updateProgress(params.repetitions, params.repetitions, "Completed");
            
        } catch (const std::exception& e) {
            pimpl_->result.status = BenchStatus::Failed;
            pimpl_->result.error_message = e.what();
            pimpl_->status = BenchStatus::Failed;
        }
        
        pimpl_->running = false;
        notifyCompletion(pimpl_->result);
    });
    
    // Отсоединить поток (чтобы не блокировать деструктор)
    pimpl_->worker_thread.detach();
}

std::string LlamaBenchRunner::buildCommandLine(const BenchTestParams& params, int context_depth) {
    std::ostringstream cmd;
    
    // Путь к llama-bench
    cmd << "\"" << pimpl_->llama_bench_path << "\"";
    
    // Модель
    cmd << " -m \"" << params.model_path << "\"";
    
    // Параметры теста
    cmd << " -p " << params.n_prompt;
    cmd << " -n " << params.n_gen;
    
    // Глубина контекста
    if (context_depth > 0) {
        cmd << " -d " << context_depth;
    } else if (!params.n_depth.empty()) {
        // Если передано несколько глубин, llama-bench сам их обработает
        std::string depths;
        for (size_t i = 0; i < params.n_depth.size(); ++i) {
            if (i > 0) depths += ",";
            depths += std::to_string(params.n_depth[i]);
        }
        if (!depths.empty()) {
            cmd << " -d " << depths;
        }
    }
    
    // Производительность
    cmd << " -b " << params.batch_size;
    cmd << " -ub " << params.ubatch_size;
    cmd << " -t " << params.threads;
    cmd << " -ngl " << params.n_gpu_layers;
    
    // Кэш
    cmd << " -ctk " << params.cache_type_k;
    cmd << " -ctv " << params.cache_type_v;
    
    // Повторения
    cmd << " -r " << params.repetitions;

    // Задержка
    if (params.delay_seconds > 0) {
        cmd << " --delay " << params.delay_seconds;
    }

    // Дополнительные опции
    if (params.flash_attn) {
        cmd << " -fa " << 1;
    }
    if (params.embeddings) {
        cmd << " -embd " << 1;
    }
    if (!params.mmap) {
        cmd << " -mmp " << 0;
    }
    if (params.main_gpu != 0) {
        cmd << " -mg " << params.main_gpu;
    }
    if (params.split_mode != 1) {
        cmd << " -sm " << params.split_mode;
    }

    // Формат вывода
    cmd << " -o " << formatToCliString(pimpl_->output_format);

    // Вывод прогресса - отключён так как мешает буферизации JSON
    // cmd << " --progress";

    // Подробный вывод
    if (pimpl_->verbose) {
        cmd << " -v";
    }

    return cmd.str();
}

int LlamaBenchRunner::executeCommand(
    const std::string& command,
    std::string& output,
    std::string& error,
    bool capture_output)
{
    std::ostringstream oss_output;
    std::ostringstream oss_error;

    // Выполнить команду с перенаправлением stderr -> stdout
    std::string full_command = command + " 2>&1";

    std::cout << "[LlamaBenchRunner] Executing command: " << full_command << std::endl;
    std::cout << "[LlamaBenchRunner] Starting execution (this may take several minutes)..." << std::endl;

    FILE* pipe = popen(full_command.c_str(), "r");
    if (!pipe) {
        error = "Failed to execute command";
        std::cerr << "[LlamaBenchRunner] ERROR: " << error << std::endl;
        return -1;
    }

    // Получить file descriptor
    int fd = fileno(pipe);
    
    // Установить неблокирующий режим
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    // Таймаут выполнения (30 минут максимум)
    const auto timeout = std::chrono::minutes(30);
    const auto start_time = std::chrono::steady_clock::now();

    std::array<char, 4096> buffer;
    int total_bytes_read = 0;
    bool has_json = false;
    std::string accumulated_line;

    try {
        while (true) {
            // Проверить общий таймаут
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            if (elapsed > timeout) {
                oss_error << "Error: Command timed out after 30 minutes";
                std::cerr << "[LlamaBenchRunner] Timeout!" << std::endl;
                break;
            }

            // Проверить отмену
            if (pimpl_->cancelled) {
                std::cout << "[LlamaBenchRunner] Cancelled by user" << std::endl;
                break;
            }

            // Использовать select для ожидания данных с таймаутом 1 секунда
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(fd, &read_fds);

            struct timeval select_timeout;
            select_timeout.tv_sec = 1;
            select_timeout.tv_usec = 0;

            int select_result = select(fd + 1, &read_fds, nullptr, nullptr, &select_timeout);

            if (select_result > 0) {
                // Данные доступны
                ssize_t bytes_read = read(fd, buffer.data(), buffer.size());

                if (bytes_read > 0) {
                    total_bytes_read += bytes_read;
                    std::string chunk(buffer.data(), bytes_read);

                    // Проверить наличие JSON
                    if (chunk.find('[') != std::string::npos || chunk.find('{') != std::string::npos) {
                        has_json = true;
                    }

                    if (capture_output) {
                        oss_output << chunk;
                        notifyOutput(chunk);
                    }
                } else if (bytes_read == 0) {
                    // EOF
                    std::cout << "[LlamaBenchRunner] EOF reached" << std::endl;
                    break;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    // Настоящая ошибка
                    oss_error << "Error: read failed";
                    std::cerr << "[LlamaBenchRunner] Read error: " << errno << std::endl;
                    break;
                }
            } else if (select_result == 0) {
                // Таймаут select - проверить не завершился ли процесс
                int status;
                pid_t result = waitpid(-1, &status, WNOHANG);
                if (result > 0) {
                    // Процесс завершился
                    std::cout << "[LlamaBenchRunner] Process completed" << std::endl;
                    break;
                }
                // Иначе продолжаем ждать
            } else {
                // Ошибка select
                if (errno != EINTR) {
                    oss_error << "Error: select failed";
                    std::cerr << "[LlamaBenchRunner] Select error" << std::endl;
                    break;
                }
            }
        }
    } catch (const std::exception& e) {
        oss_error << "Error: " << e.what();
        std::cerr << "[LlamaBenchRunner] Exception: " << e.what() << std::endl;
    }

    pimpl_->process_id = -1;

    int exit_code = pclose(pipe);

    std::cout << "[LlamaBenchRunner] Execution complete: exit_code=" << exit_code
              << ", total_bytes=" << total_bytes_read
              << ", has_json=" << (has_json ? "yes" : "no") << std::endl;
    std::cout << "[LlamaBenchRunner] Output length: " << oss_output.str().length() << std::endl;

    output = oss_output.str();
    error = oss_error.str();

    return exit_code;
}

BenchRunResult LlamaBenchRunner::parseOutput(const std::string& output, const std::string& error) {
    BenchRunResult result;

    // Если есть ошибка выполнения, вернуть失败
    if (!error.empty() && output.empty()) {
        result.status = BenchStatus::Failed;
        result.error_message = error;
        return result;
    }

    // Отладочный вывод
    std::cout << "[LlamaBenchParser] Raw output length: " << output.length() << std::endl;
    std::cout << "[LlamaBenchParser] Raw output preview:" << std::endl;
    size_t preview_len = std::min(output.length(), static_cast<size_t>(1000));
    std::cout << output.substr(0, preview_len) << std::endl;
    std::cout << "[LlamaBenchParser] --- END RAW OUTPUT ---" << std::endl;

    // Проверить на наличие ключевых слов ошибки в выводе
    if (output.find("error") != std::string::npos ||
        output.find("Error") != std::string::npos ||
        output.find("failed") != std::string::npos ||
        output.find("Failed") != std::string::npos) {

        result.status = BenchStatus::Failed;
        result.error_message = output;
        return result;
    }

    // Очистить вывод от сообщений прогресса (llama-bench: ...)
    std::string clean_output;
    std::istringstream iss(output);
    std::string line;
    bool json_started = false;
    
    while (std::getline(iss, line)) {
        // Пропустить строки с сообщениями прогресса
        if (line.find("llama-bench:") != std::string::npos ||
            line.find("load_backend:") != std::string::npos) {
            continue;
        }
        
        // Найти начало JSON
        if (!json_started) {
            size_t json_start = line.find('[');
            if (json_start != std::string::npos) {
                json_started = true;
                clean_output += line.substr(json_start);
                clean_output += '\n';
            } else {
                size_t json_obj_start = line.find('{');
                if (json_obj_start != std::string::npos) {
                    json_started = true;
                    clean_output += line.substr(json_obj_start);
                    clean_output += '\n';
                }
            }
        } else {
            clean_output += line;
            clean_output += '\n';
        }
    }
    
    // Если JSON не найден, попробовать использовать весь вывод
    if (clean_output.empty()) {
        clean_output = output;
    }
    
    // Отладочный вывод очищенного JSON
    std::cout << "[LlamaBenchParser] Clean output length: " << clean_output.length() << std::endl;
    std::cout << "[LlamaBenchParser] Clean output preview:" << std::endl;
    preview_len = std::min(clean_output.length(), static_cast<size_t>(1000));
    std::cout << clean_output.substr(0, preview_len) << std::endl;
    std::cout << "[LlamaBenchParser] --- END CLEAN OUTPUT ---" << std::endl;

    // Базовый парсинг метрик из JSON вывода
    // llama-bench возвращает массив из ДВУХ объектов:
    // 1) Тест промпта: n_prompt > 0, n_gen = 0
    // 2) Тест генерации: n_prompt = 0, n_gen > 0
    // Нужно обработать оба и объединить метрики
    
    double prompt_tps = 0.0;
    double gen_tps = 0.0;
    
    // Найти все вхождения avg_ts (для каждого объекта JSON)
    auto findAllAvgTs = [&clean_output]() -> std::vector<double> {
        std::vector<double> values;
        std::string search = "\"avg_ts\":";
        size_t pos = 0;
        
        while ((pos = clean_output.find(search, pos)) != std::string::npos) {
            pos += search.length();
            while (pos < clean_output.length() && (clean_output[pos] == ' ' || clean_output[pos] == '\t')) {
                ++pos;
            }
            size_t end = clean_output.find_first_of(",]", pos);
            if (end == std::string::npos) break;
            
            std::string value = clean_output.substr(pos, end - pos);
            try {
                values.push_back(std::stod(value));
            } catch (...) {
                values.push_back(0.0);
            }
            pos = end;
        }
        
        return values;
    };
    
    auto findValue = [&clean_output](const std::string& key) -> double {
        std::string search = "\"" + key + "\":";
        size_t pos = clean_output.find(search);
        if (pos == std::string::npos) {
            return 0.0;
        }

        pos += search.length();
        while (pos < clean_output.length() && (clean_output[pos] == ' ' || clean_output[pos] == '\t')) {
            ++pos;
        }

        size_t end = clean_output.find_first_of(",}", pos);
        if (end == std::string::npos) {
            return 0.0;
        }

        std::string value = clean_output.substr(pos, end - pos);
        try {
            return std::stod(value);
        } catch (...) {
            return 0.0;
        }
    };

    // Найти все значения avg_ts
    std::vector<double> avg_ts_values = findAllAvgTs();
    
    // Первое значение - prompt TPS, второе - gen TPS
    if (avg_ts_values.size() >= 1) {
        prompt_tps = avg_ts_values[0];
    }
    if (avg_ts_values.size() >= 2) {
        gen_tps = avg_ts_values[1];
    }
    
    result.prompt_tokens_per_sec = prompt_tps;
    result.gen_tokens_per_sec = gen_tps;
    
    // Также попробовать стандартные поля если есть
    double prompt_ts = findValue("prompt_tokens_per_sec");
    double gen_ts = findValue("generation_tokens_per_sec");
    if (prompt_ts > 0) result.prompt_tokens_per_sec = prompt_ts;
    if (gen_ts > 0) result.gen_tokens_per_sec = gen_ts;
    
    result.prompt_ms_total = findValue("prompt_ms_total");
    result.gen_ms_total = findValue("generation_ms_total");
    result.ttft_ms = findValue("ttft_ms");

    // Извлечь model_path если есть
    size_t model_pos = clean_output.find("\"model\":");
    if (model_pos == std::string::npos) {
        model_pos = clean_output.find("\"model_filename\":");
    }

    if (model_pos != std::string::npos) {
        size_t key_end = clean_output.find(':', model_pos);
        if (key_end != std::string::npos) {
            model_pos = key_end + 1;
            while (model_pos < clean_output.length() && (clean_output[model_pos] == ' ' || clean_output[model_pos] == '\t' || clean_output[model_pos] == '"')) {
                ++model_pos;
            }
            size_t model_end = clean_output.find('"', model_pos);
            if (model_end != std::string::npos) {
                result.model_path = clean_output.substr(model_pos, model_end - model_pos);
                // Извлечь имя из пути
                size_t name_pos = result.model_path.find_last_of("/\\");
                result.model_name = (name_pos != std::string::npos) ?
                    result.model_path.substr(name_pos + 1) : result.model_path;
            }
        }
    }

    // Если model_name всё ещё пустой, попробовать извлечь из model_filename
    if (result.model_name.empty() && !result.model_path.empty()) {
        size_t name_pos = result.model_path.find_last_of("/\\");
        result.model_name = (name_pos != std::string::npos) ?
            result.model_path.substr(name_pos + 1) : result.model_path;

        // Удалить расширение
        size_t ext_pos = result.model_name.find_last_of('.');
        if (ext_pos != std::string::npos) {
            result.model_name = result.model_name.substr(0, ext_pos);
        }
    }

    // Установить profile_name из model_name если не задан
    if (result.profile_name.empty() && !result.model_name.empty()) {
        result.profile_name = result.model_name;
    }

    // Если найдены метрики, считать успешным
    // ВАЖНО: prompt_tokens_per_sec > 0 достаточно для успеха (генерация может быть отключена)
    if (result.prompt_tokens_per_sec > 0) {
        result.status = BenchStatus::Completed;
        result.repetitions_completed = pimpl_->current_params.repetitions;
        
        // Если gen_tokens_per_sec=0, вывести предупреждение
        if (result.gen_tokens_per_sec <= 0) {
            std::cout << "[LlamaBenchParser] WARNING: Generation TPS is 0, only prompt test completed" << std::endl;
        }
    } else {
        result.status = BenchStatus::Failed;
        result.error_message = "No metrics found in output. Output length: " +
                               std::to_string(output.length());
    }

    return result;
}

void LlamaBenchRunner::updateProgress(int current, int total, const std::string& status) {
    int percent = (total > 0) ? (current * 100 / total) : 0;
    pimpl_->progress = percent;
    
    if (pimpl_->progress_callback) {
        pimpl_->progress_callback(current, total, status);
    }
}

void LlamaBenchRunner::notifyOutput(const std::string& line) {
    if (pimpl_->output_callback) {
        pimpl_->output_callback(line);
    }
}

void LlamaBenchRunner::notifyCompletion(const BenchRunResult& result) {
    if (pimpl_->completion_callback) {
        pimpl_->completion_callback(result);
    }
}

} // namespace bench
} // namespace llama_gui
