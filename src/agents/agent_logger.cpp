#include "agents/agent_logger.h"
#include <chrono>
#include <iostream>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace agents {

// ============================================================================
// LogEntry implementation
// ============================================================================

std::string LogEntry::to_string() const {
    std::ostringstream oss;
    
    // Форматирование времени
    int64_t secs = timestamp_ms / 1000;
    int64_t ms = timestamp_ms % 1000;
    
    oss << "[" << std::setw(2) << std::setfill('0') 
        << (secs / 3600) << ":"
        << std::setw(2) << std::setfill('0')
        << ((secs % 3600) / 60) << ":"
        << std::setw(2) << std::setfill('0')
        << (secs % 60) << "."
        << std::setw(3) << std::setfill('0') << ms
        << "]";
    
    // Уровень
    const char* level_str = "";
    switch (level) {
        case LogLevel::DEBUG:    level_str = "DEBUG"; break;
        case LogLevel::INFO:     level_str = "INFO"; break;
        case LogLevel::WARNING:  level_str = "WARN"; break;
        case LogLevel::ERROR:    level_str = "ERROR"; break;
        case LogLevel::CRITICAL: level_str = "CRIT"; break;
    }
    
    oss << "[" << level_str << "]";
    
    // Агент
    oss << "[" << agent_name << "]";
    
    // Сообщение
    oss << " " << message;
    
    if (!context.empty()) {
        oss << " (" << context << ")";
    }
    
    return oss.str();
}

// ============================================================================
// AgentLogger implementation
// ============================================================================

/**
 * @brief Внутренняя реализация AgentLogger
 */
class AgentLogger::Impl {
public:
    std::deque<LogEntry> entries;
    LogLevel min_level = LogLevel::DEBUG;
    std::string log_file;
    std::ofstream file_stream;
    std::function<void(const LogEntry&)> callback;
    mutable std::mutex mutex;
    
    int64_t start_time_ms;
    
    Impl() {
        start_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    
    int64_t get_current_time_ms() const {
        auto now = std::chrono::steady_clock::now();
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        return now_ms - start_time_ms;
    }
};

// ============================================================================

AgentLogger::AgentLogger() : impl_(std::make_unique<Impl>()) {}

AgentLogger::~AgentLogger() {
    disable_file_logging();
}

void AgentLogger::set_min_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->min_level = level;
}

LogLevel AgentLogger::get_min_level() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->min_level;
}

void AgentLogger::log(const std::string& agent_name, LogLevel level,
                       const std::string& message) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    // Проверка уровня
    if (static_cast<int>(level) < static_cast<int>(impl_->min_level)) {
        return;
    }
    
    // Создание записи
    LogEntry entry;
    entry.timestamp_ms = impl_->get_current_time_ms();
    entry.agent_name = agent_name;
    entry.level = level;
    entry.message = message;
    
    // Добавление в очередь
    impl_->entries.push_back(entry);
    
    // Ограничение размера (последние 10000 записей)
    while (impl_->entries.size() > 10000) {
        impl_->entries.pop_front();
    }
    
    // Вывод в консоль
    std::cout << entry.to_string() << std::endl;
    
    // Запись в файл
    if (impl_->file_stream.is_open()) {
        impl_->file_stream << entry.to_string() << std::endl;
        impl_->file_stream.flush();
    }
    
    // Колбэк
    if (impl_->callback) {
        impl_->callback(entry);
    }
}

void AgentLogger::debug(const std::string& agent_name, const std::string& msg) {
    log(agent_name, LogLevel::DEBUG, msg);
}

void AgentLogger::info(const std::string& agent_name, const std::string& msg) {
    log(agent_name, LogLevel::INFO, msg);
}

void AgentLogger::warning(const std::string& agent_name, const std::string& msg) {
    log(agent_name, LogLevel::WARNING, msg);
}

void AgentLogger::error(const std::string& agent_name, const std::string& msg) {
    log(agent_name, LogLevel::ERROR, msg);
}

void AgentLogger::critical(const std::string& agent_name, const std::string& msg) {
    log(agent_name, LogLevel::CRITICAL, msg);
}

std::vector<LogEntry> AgentLogger::get_entries(const LogFilter& filter) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    std::vector<LogEntry> result;
    
    for (const auto& entry : impl_->entries) {
        // Фильтр по агенту
        if (!filter.agent_name.empty() && 
            entry.agent_name != filter.agent_name) {
            continue;
        }
        
        // Фильтр по уровню
        if (static_cast<int>(entry.level) < static_cast<int>(filter.min_level)) {
            continue;
        }
        
        // Фильтр по времени
        if (entry.timestamp_ms < filter.from_timestamp) {
            continue;
        }
        if (filter.to_timestamp >= 0 && 
            entry.timestamp_ms > filter.to_timestamp) {
            continue;
        }
        
        result.push_back(entry);
    }
    
    return result;
}

std::vector<LogEntry> AgentLogger::get_agent_logs(
        const std::string& agent_name,
        int limit) const {
    LogFilter filter;
    filter.agent_name = agent_name;
    filter.min_level = LogLevel::DEBUG;
    
    auto entries = get_entries(filter);
    
    if (limit > 0 && static_cast<int>(entries.size()) > limit) {
        return std::vector<LogEntry>(
            entries.end() - limit, entries.end());
    }
    
    return entries;
}

std::vector<LogEntry> AgentLogger::get_by_level(LogLevel level) const {
    LogFilter filter;
    filter.min_level = level;
    return get_entries(filter);
}

void AgentLogger::clear(const std::string& agent_name) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    if (agent_name.empty()) {
        impl_->entries.clear();
    } else {
        impl_->entries.erase(
            std::remove_if(impl_->entries.begin(), impl_->entries.end(),
                [&agent_name](const LogEntry& e) {
                    return e.agent_name == agent_name;
                }),
            impl_->entries.end());
    }
}

size_t AgentLogger::size() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->entries.size();
}

size_t AgentLogger::size(const std::string& agent_name) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    size_t count = 0;
    for (const auto& entry : impl_->entries) {
        if (entry.agent_name == agent_name) {
            count++;
        }
    }
    return count;
}

bool AgentLogger::export_to_file(const std::string& filename,
                                  const LogFilter& filter) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        return false;
    }
    
    auto entries = get_entries(filter);
    
    for (const auto& entry : entries) {
        file << entry.to_string() << "\n";
    }
    
    return true;
}

void AgentLogger::enable_file_logging(const std::string& filename) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    disable_file_logging();
    
    impl_->log_file = filename;
    impl_->file_stream.open(filename, std::ios::app);
    
    if (impl_->file_stream.is_open()) {
        impl_->file_stream << "\n=== Logger started ===\n";
    }
}

void AgentLogger::disable_file_logging() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    if (impl_->file_stream.is_open()) {
        impl_->file_stream.close();
    }
    impl_->log_file.clear();
}

void AgentLogger::set_callback(std::function<void(const LogEntry&)> callback) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->callback = std::move(callback);
}

std::unordered_map<std::string, size_t> AgentLogger::get_stats() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    std::unordered_map<std::string, size_t> stats;
    
    for (const auto& entry : impl_->entries) {
        stats[entry.agent_name]++;
    }
    
    return stats;
}

} // namespace agents
