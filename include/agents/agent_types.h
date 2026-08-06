#pragma once

#include <string>
#include <cstdint>

namespace agents {

/**
 * @brief Статус выполнения операции агента
 */
enum class AgentStatus {
    OK = 0,           ///< Успешно
    ERROR = 1,        ///< Ошибка выполнения
    TIMEOUT = 2,      ///< Превышено время ожидания
    CANCELLED = 3,    ///< Операция отменена
    NOT_FOUND = 4,    ///< Агент не найден
    PERMISSION_DENIED = 5,  ///< Нет прав доступа
    ALREADY_EXISTS = 6,     ///< Уже существует
    INVALID_STATE = 7,      ///< Неверное состояние
    UNAVAILABLE = 8         ///< Агент недоступен
};

/**
 * @brief Преобразование статуса в строку
 */
inline const char* agent_status_to_string(AgentStatus status) {
    switch (status) {
        case AgentStatus::OK: return "OK";
        case AgentStatus::ERROR: return "ERROR";
        case AgentStatus::TIMEOUT: return "TIMEOUT";
        case AgentStatus::CANCELLED: return "CANCELLED";
        case AgentStatus::NOT_FOUND: return "NOT_FOUND";
        case AgentStatus::PERMISSION_DENIED: return "PERMISSION_DENIED";
        case AgentStatus::ALREADY_EXISTS: return "ALREADY_EXISTS";
        case AgentStatus::INVALID_STATE: return "INVALID_STATE";
        case AgentStatus::UNAVAILABLE: return "UNAVAILABLE";
        default: return "UNKNOWN";
    }
}

/**
 * @brief Уровни логирования агентов
 */
enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARNING = 2,
    ERROR = 3,
    CRITICAL = 4
};

/**
 * @brief Типы данных для передачи между агентами
 */
enum class DataType {
    NONE = 0,
    STRING = 1,
    NUMBER = 2,
    BOOLEAN = 3,
    ARRAY = 4,
    OBJECT = 5,
    BINARY = 6
};

/**
 * @brief Версия API агентов
 * Изменяется при breaking changes в интерфейсе
 */
constexpr const char* AGENT_API_VERSION = "1.0.0";
constexpr int AGENT_API_VERSION_MAJOR = 1;
constexpr int AGENT_API_VERSION_MINOR = 0;
constexpr int AGENT_API_VERSION_PATCH = 0;

} // namespace agents
