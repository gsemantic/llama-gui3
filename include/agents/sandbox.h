#pragma once

#include "agent_types.h"
#include <string>
#include <memory>
#include <functional>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>

namespace agents {

/**
 * @brief Статус выполнения в песочнице
 */
enum class SandboxStatus {
    IDLE,       ///< Бездействует
    RUNNING,    ///< Выполняется
    COMPLETED,  ///< Завершено
    TIMEOUT,    ///< Превышено время
    ERROR,      ///< Ошибка
    TERMINATED  ///< Принудительно остановлено
};

/**
 * @brief Результат выполнения в песочнице
 */
struct SandboxResult {
    SandboxStatus status;
    int exit_code;
    std::string output;
    std::string error_output;
    long long execution_time_ms;
    long long memory_used_kb;

    static SandboxResult success(const std::string& output, 
                                  long long time_ms,
                                  long long memory_kb) {
        return {SandboxStatus::COMPLETED, 0, output, "", time_ms, memory_kb};
    }

    static SandboxResult timeout(long long time_ms) {
        return {SandboxStatus::TIMEOUT, -1, "", "Execution timeout", time_ms, 0};
    }

    static SandboxResult error(const std::string& msg, int code = -1) {
        return {SandboxStatus::ERROR, code, "", msg, 0, 0};
    }

    static SandboxResult terminated() {
        return {SandboxStatus::TERMINATED, -1, "", "Terminated", 0, 0};
    }
};

/**
 * @brief Конфигурация песочницы
 */
struct SandboxConfig {
    int timeout_ms = 30000;           ///< Таймаут выполнения
    int max_memory_mb = 512;          ///< Максимум памяти (MB)
    int max_output_size_kb = 1024;    ///< Максимум вывода (KB)
    bool allow_network = false;       ///< Запрет сети
    bool allow_filesystem = false;    ///< Ограниченный доступ к ФС
    std::string working_dir;          ///< Рабочая директория
    std::string chroot_dir;           ///< Корень (если используется)
};

/**
 * @brief Песочница для изоляции выполнения агентов
 * 
 * Обеспечивает безопасное выполнение кода с ограничениями:
 * - Таймауты выполнения
 * - Ограничение памяти
 * - Ограничение вывода
 * - Изоляция от системы
 */
class Sandbox {
public:
    Sandbox();
    explicit Sandbox(const SandboxConfig& config);
    ~Sandbox();

    /**
     * @brief Установка конфигурации
     */
    void set_config(const SandboxConfig& config);

    /**
     * @brief Получение конфигурации
     */
    const SandboxConfig& get_config() const;

    /**
     * @brief Выполнение функции с ограничениями
     * @param func Функция для выполнения
     * @return Результат выполнения
     * 
     * @note На Linux используется seccomp-bpf для изоляции
     * @note На Windows используется Job Object
     */
    SandboxResult execute(const std::function<int()>& func);

    /**
     * @brief Выполнение команды в песочнице
     * @param command Команда
     * @param args Аргументы
     * @return Результат выполнения
     */
    SandboxResult execute_command(const std::string& command,
                                   const std::vector<std::string>& args);

    /**
     * @brief Остановка выполнения
     */
    void terminate();

    /**
     * @brief Проверка статуса
     */
    SandboxStatus get_status() const;

    /**
     * @brief Проверка выполнения
     */
    bool is_running() const;

    /**
     * @brief Получение последнего результата
     */
    const SandboxResult& get_last_result() const;

    /**
     * @brief Сброс состояния
     */
    void reset();

    /**
     * @brief Выполнение с таймаутом
     * @param func Функция
     * @param timeout_ms Таймаут в мс
     * @return Результат
     */
    static SandboxResult run_with_timeout(
        const std::function<int()>& func,
        int timeout_ms);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace agents
