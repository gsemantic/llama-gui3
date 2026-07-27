#include "agents/agent_context.h"
#include "agents/agent_registry.h"
#include <chrono>
#include <iostream>
#include <mutex>

namespace agents {

/**
 * @brief Внутренняя реализация AgentContext
 */
class AgentContext::Impl {
public:
    std::atomic<bool> cancelled{false};
    int timeout_ms = 30000;  // 30 секунд по умолчанию
    std::chrono::steady_clock::time_point start_time;
    
    mutable std::mutex state_mutex;
    nlohmann::json state;
    
    std::string plugins_dir = "plugins";
    std::string data_dir = "data/agents";
    
    AgentRegistry* registry = nullptr;
    
    Impl() : start_time(std::chrono::steady_clock::now()) {}
};

// ============================================================================

AgentContext::AgentContext() : impl_(std::make_unique<Impl>()) {}

AgentContext::~AgentContext() = default;

AgentResult AgentContext::call_agent(const AgentRequest& request) {
    if (is_cancelled()) {
        return AgentResult::cancelled("Operation was cancelled");
    }
    
    if (is_timeout()) {
        return AgentResult::error("Operation timeout exceeded");
    }
    
    if (!impl_->registry) {
        return AgentResult::error("Agent registry not set");
    }
    
    return impl_->registry->execute(request);
}

AgentResult AgentContext::call_agent(const std::string& agent_name,
                                      const std::string& action,
                                      const nlohmann::json& params) {
    AgentRequest request(agent_name, action, params);
    return call_agent(request);
}

bool AgentContext::is_cancelled() const {
    return impl_->cancelled.load();
}

void AgentContext::request_cancel() {
    impl_->cancelled.store(true);
}

bool AgentContext::is_timeout() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - impl_->start_time).count();
    return elapsed > impl_->timeout_ms;
}

void AgentContext::set_timeout_ms(int timeout_ms) {
    impl_->timeout_ms = timeout_ms;
}

void AgentContext::log(const std::string& agent_name, LogLevel level,
                       const std::string& message) {
    const char* level_str = "";
    switch (level) {
        case LogLevel::DEBUG:    level_str = "DEBUG"; break;
        case LogLevel::INFO:     level_str = "INFO"; break;
        case LogLevel::WARNING:  level_str = "WARNING"; break;
        case LogLevel::ERROR:    level_str = "ERROR"; break;
        case LogLevel::CRITICAL: level_str = "CRITICAL"; break;
    }
    
    std::cout << "[AGENT:" << agent_name << ":" << level_str << "] " 
              << message << std::endl;
}

void AgentContext::debug(const std::string& agent_name, const std::string& msg) {
    log(agent_name, LogLevel::DEBUG, msg);
}

void AgentContext::info(const std::string& agent_name, const std::string& msg) {
    log(agent_name, LogLevel::INFO, msg);
}

void AgentContext::warning(const std::string& agent_name, const std::string& msg) {
    log(agent_name, LogLevel::WARNING, msg);
}

void AgentContext::error(const std::string& agent_name, const std::string& msg) {
    log(agent_name, LogLevel::ERROR, msg);
}

nlohmann::json AgentContext::get_state(const std::string& key) const {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    if (impl_->state.contains(key)) {
        return impl_->state[key];
    }
    return nullptr;
}

void AgentContext::set_state(const std::string& key, const nlohmann::json& value) {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    impl_->state[key] = value;
}

bool AgentContext::has_state(const std::string& key) const {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    return impl_->state.contains(key);
}

std::string AgentContext::get_plugins_dir() const {
    return impl_->plugins_dir;
}

void AgentContext::set_plugins_dir(const std::string& dir) {
    impl_->plugins_dir = dir;
}

std::string AgentContext::get_data_dir() const {
    return impl_->data_dir;
}

void AgentContext::set_data_dir(const std::string& dir) {
    impl_->data_dir = dir;
}

nlohmann::json AgentContext::get_agent_config(const std::string& agent_name) const {
    // TODO: Загрузка из agents_config.json
    return nlohmann::json::object();
}

void AgentContext::set_registry(AgentRegistry* registry) {
    impl_->registry = registry;
}

AgentRegistry* AgentContext::get_registry() const {
    return impl_->registry;
}

} // namespace agents
