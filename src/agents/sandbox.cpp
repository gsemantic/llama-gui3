#include "agents/sandbox.h"
#include <iostream>
#include <sstream>
#include <future>

namespace agents {

/**
 * @brief Внутренняя реализация Sandbox
 */
class Sandbox::Impl {
public:
    SandboxConfig config;
    std::atomic<SandboxStatus> status{SandboxStatus::IDLE};
    std::atomic<bool> terminate_requested{false};
    SandboxResult last_result;
    mutable std::mutex mutex;

    Impl() = default;
};

// ============================================================================

Sandbox::Sandbox() : impl_(std::make_unique<Impl>()) {}

Sandbox::Sandbox(const SandboxConfig& config) 
    : impl_(std::make_unique<Impl>()) {
    impl_->config = config;
}

Sandbox::~Sandbox() {
    terminate();
}

void Sandbox::set_config(const SandboxConfig& config) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->config = config;
}

const SandboxConfig& Sandbox::get_config() const {
    return impl_->config;
}

SandboxResult Sandbox::execute(const std::function<int()>& func) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    impl_->status = SandboxStatus::RUNNING;
    impl_->terminate_requested = false;
    
    auto start = std::chrono::steady_clock::now();
    
    // Запускаем функцию в отдельном потоке с таймаутом
    auto future = std::async(std::launch::async, func);
    
    // Ждём завершения или таймаута
    auto timeout = std::chrono::milliseconds(impl_->config.timeout_ms);
    
    if (future.wait_for(timeout) == std::future_status::timeout) {
        impl_->status = SandboxStatus::TIMEOUT;
        impl_->last_result = SandboxResult::timeout(impl_->config.timeout_ms);
        return impl_->last_result;
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - start).count();
    
    try {
        int exit_code = future.get();
        
        if (impl_->terminate_requested) {
            impl_->status = SandboxStatus::TERMINATED;
            impl_->last_result = SandboxResult::terminated();
        } else {
            impl_->status = SandboxStatus::COMPLETED;
            impl_->last_result = SandboxResult::success("", duration, 0);
            impl_->last_result.exit_code = exit_code;
        }
    } catch (const std::exception& e) {
        impl_->status = SandboxStatus::ERROR;
        impl_->last_result = SandboxResult::error(e.what());
    }
    
    return impl_->last_result;
}

SandboxResult Sandbox::execute_command(const std::string& command,
                                        const std::vector<std::string>& args) {
    // Простая реализация через system()
    // В production нужно использовать execve() с ограничениями
    
    std::ostringstream cmd_stream;
    cmd_stream << command;
    
    for (const auto& arg : args) {
        cmd_stream << " " << arg;
    }
    
    std::string cmd = cmd_stream.str();
    
    return execute([cmd]() {
        int result = std::system(cmd.c_str());
        return result;
    });
}

void Sandbox::terminate() {
    impl_->terminate_requested = true;
    impl_->status = SandboxStatus::TERMINATED;
}

SandboxStatus Sandbox::get_status() const {
    return impl_->status.load();
}

bool Sandbox::is_running() const {
    return impl_->status == SandboxStatus::RUNNING;
}

const SandboxResult& Sandbox::get_last_result() const {
    return impl_->last_result;
}

void Sandbox::reset() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->status = SandboxStatus::IDLE;
    impl_->terminate_requested = false;
    impl_->last_result = SandboxResult::success("", 0, 0);
}

SandboxResult Sandbox::run_with_timeout(
        const std::function<int()>& func,
        int timeout_ms) {
    
    auto start = std::chrono::steady_clock::now();
    auto future = std::async(std::launch::async, func);
    
    auto timeout = std::chrono::milliseconds(timeout_ms);
    
    if (future.wait_for(timeout) == std::future_status::timeout) {
        return SandboxResult::timeout(timeout_ms);
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - start).count();
    
    try {
        int exit_code = future.get();
        return SandboxResult::success("", duration, 0);
    } catch (const std::exception& e) {
        return SandboxResult::error(e.what());
    }
}

} // namespace agents
