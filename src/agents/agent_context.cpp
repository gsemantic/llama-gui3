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
    int timeout_ms = 30000;  // 30 seconds default
    std::chrono::steady_clock::time_point start_time;
    
    mutable std::mutex state_mutex;
    nlohmann::json state;
    
    std::string plugins_dir = "plugins";
    std::string data_dir = "data/agents";
    std::string project_root;
    
    AgentRegistry* registry = nullptr;
    LlmCompleteFn llm_complete_fn;
    
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

void AgentContext::set_llm_complete(LlmCompleteFn fn) {
    impl_->llm_complete_fn = std::move(fn);
}

std::string AgentContext::llm_complete(const std::string& system_prompt,
                                       const std::string& user_prompt) {
    if (!impl_->llm_complete_fn) {
        return "";
    }
    return impl_->llm_complete_fn(system_prompt, user_prompt);
}

bool AgentContext::has_llm() const {
    return static_cast<bool>(impl_->llm_complete_fn);
}

void AgentContext::set_project_root(const std::string& root) {
    impl_->project_root = root;
}

std::string AgentContext::get_project_root() const {
    return impl_->project_root;
}

bool AgentContext::is_within_project(const std::string& path) const {
    if (impl_->project_root.empty()) return true;  // no restriction

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path abs = fs::absolute(path, ec);
    if (ec) return false;

    fs::path root = fs::absolute(impl_->project_root, ec);
    if (ec) return false;

    // Check if abs starts with root
    auto root_it = root.begin();
    auto abs_it = abs.begin();
    for (; root_it != root.end(); ++root_it, ++abs_it) {
        if (abs_it == abs.end()) return false;
        if (*root_it != *abs_it) return false;
    }
    return true;
}

} // namespace agents
