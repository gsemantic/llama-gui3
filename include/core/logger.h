#pragma once

#include <string>
#include <iostream>
#include <sstream>

namespace llama_gui {
namespace core {

/**
 * Logger with two modes: User (minimal) and Debug (verbose)
 */
class Logger {
public:
    enum class Level {
        None = 0,    // No logging
        Error = 1,   // Only errors
        Warning = 2, // Errors + warnings
        Info = 3,    // Errors + warnings + info
        Debug = 4    // All logs including debug
    };

    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    void set_level(Level level) {
        level_ = level;
    }

    Level get_level() const {
        return level_;
    }

    void set_debug_mode(bool debug) {
        level_ = debug ? Level::Debug : Level::Warning;
    }

    bool is_debug_mode() const {
        return level_ >= Level::Debug;
    }

    // Error logging (always enabled if level >= Error)
    void error(const std::string& message) {
        if (level_ >= Level::Error) {
            std::cerr << "❌ ERROR: " << message << std::endl;
        }
    }

    template<typename... Args>
    void error(const std::string& format, Args... args) {
        if (level_ >= Level::Error) {
            std::cerr << "❌ ERROR: " << format << std::endl;
        }
    }

    // Warning logging
    void warning(const std::string& message) {
        if (level_ >= Level::Warning) {
            std::cerr << "⚠️  WARNING: " << message << std::endl;
        }
    }

    // Info logging
    void info(const std::string& message) {
        if (level_ >= Level::Info) {
            std::cout << "ℹ️  INFO: " << message << std::endl;
        }
    }

    // Debug logging (only in debug mode)
    void debug(const std::string& message) {
        if (level_ >= Level::Debug) {
            std::cout << "🔧 DEBUG: " << message << std::endl;
        }
    }

    template<typename T>
    void debug(const std::string& prefix, const T& value) {
        if (level_ >= Level::Debug) {
            std::cout << "🔧 DEBUG: " << prefix << " = " << value << std::endl;
        }
    }

    // Stream-like logging
    class LogStream {
    public:
        LogStream(Level level, const std::string& prefix) 
            : level_(level), prefix_(prefix), enabled_(level <= Logger::instance().level_) {
            if (enabled_) {
                stream_ << prefix << " ";
            }
        }

        template<typename T>
        LogStream& operator<<(const T& value) {
            if (enabled_) {
                stream_ << value;
            }
            return *this;
        }

        ~LogStream() {
            if (enabled_) {
                if (level_ == Level::Error) {
                    std::cerr << stream_.str() << std::endl;
                } else {
                    std::cout << stream_.str() << std::endl;
                }
            }
        }

    private:
        Level level_;
        std::string prefix_;
        bool enabled_;
        std::ostringstream stream_;
    };

    LogStream stream(Level level, const std::string& prefix) {
        return LogStream(level, prefix);
    }

private:
    Logger() : level_(Level::Warning) {} // Default: warnings only
    Level level_;
};

// Convenience macros
#define LOG_ERROR(msg) llama_gui::core::Logger::instance().error(msg)
#define LOG_WARNING(msg) llama_gui::core::Logger::instance().warning(msg)
#define LOG_INFO(msg) llama_gui::core::Logger::instance().info(msg)
#define LOG_DEBUG(msg) llama_gui::core::Logger::instance().debug(msg)

#define LOG_DEBUG_VAR(var) llama_gui::core::Logger::instance().debug(#var, var)

} // namespace core
} // namespace llama_gui
