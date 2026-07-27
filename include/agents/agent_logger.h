#pragma once

#include "agent_types.h"
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <functional>
#include <fstream>

namespace agents {

/**
 * @brief Запись лога
 */
struct LogEntry {
    int64_t timestamp_ms;     ///< Метка времени (ms от старта)
    std::string agent_name;   ///< Имя агента
    LogLevel level;           ///< Уровень лога
    std::string message;      ///< Сообщение
    std::string context;      ///< Дополнительный контекст

    std::string to_string() const;
};

/**
 * @brief Фильтр для логов
 */
struct LogFilter {
    std::string agent_name;       ///< Фильтр по агенту
    LogLevel min_level = LogLevel::DEBUG;  ///< Минимальный уровень
    int64_t from_timestamp = 0;   ///< С этого времени
    int64_t to_timestamp = -1;    ///< По это время (-1 = до конца)
};

/**
 * @brief Логгер для системы агентов
 * 
 * Предоставляет:
 * - Раздельное логирование для каждого агента
 * - Уровни логирования
 * - Фильтрацию и поиск
 * - Экспорт в файл
 */
class AgentLogger {
public:
    AgentLogger();
    ~AgentLogger();

    /**
     * @brief Установка минимального уровня логирования
     * @param level Минимальный уровень
     */
    void set_min_level(LogLevel level);

    /**
     * @brief Получение минимального уровня
     */
    LogLevel get_min_level() const;

    /**
     * @brief Логирование сообщения
     * @param agent_name Имя агента
     * @param level Уровень
     * @param message Сообщение
     */
    void log(const std::string& agent_name, LogLevel level,
             const std::string& message);

    /**
     * @brief Логирование DEBUG
     */
    void debug(const std::string& agent_name, const std::string& msg);

    /**
     * @brief Логирование INFO
     */
    void info(const std::string& agent_name, const std::string& msg);

    /**
     * @brief Логирование WARNING
     */
    void warning(const std::string& agent_name, const std::string& msg);

    /**
     * @brief Логирование ERROR
     */
    void error(const std::string& agent_name, const std::string& msg);

    /**
     * @brief Логирование CRITICAL
     */
    void critical(const std::string& agent_name, const std::string& msg);

    /**
     * @brief Получение записей лога
     * @param filter Фильтр
     * @return Записи лога
     */
    std::vector<LogEntry> get_entries(const LogFilter& filter = {}) const;

    /**
     * @brief Получение записей для агента
     * @param agent_name Имя агента
     * @param limit Максимум записей (0 = без ограничений)
     * @return Записи лога
     */
    std::vector<LogEntry> get_agent_logs(const std::string& agent_name,
                                          int limit = 100) const;

    /**
     * @brief Получение записей по уровню
     * @param level Уровень
     * @return Записи лога
     */
    std::vector<LogEntry> get_by_level(LogLevel level) const;

    /**
     * @brief Очистка логов
     * @param agent_name Имя агента (пустой = все агенты)
     */
    void clear(const std::string& agent_name = "");

    /**
     * @brief Получение количества записей
     */
    size_t size() const;

    /**
     * @brief Получение количества записей агента
     */
    size_t size(const std::string& agent_name) const;

    /**
     * @brief Экспорт логов в файл
     * @param filename Имя файла
     * @param filter Фильтр (опционально)
     * @return true если успешно
     */
    bool export_to_file(const std::string& filename,
                         const LogFilter& filter = {});

    /**
     * @brief Включение экспорта в файл
     * @param filename Имя файла для логирования
     */
    void enable_file_logging(const std::string& filename);

    /**
     * @brief Отключение экспорта в файл
     */
    void disable_file_logging();

    /**
     * @brief Установка колбэка для логов
     * @param callback Функция обратного вызова
     */
    void set_callback(std::function<void(const LogEntry&)> callback);

    /**
     * @brief Статистика по агентам
     * @return Карта агент -> количество записей
     */
    std::unordered_map<std::string, size_t> get_stats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace agents
