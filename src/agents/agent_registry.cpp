#include "agents/agent_registry.h"
#include "agents/agent_context.h"
#include <algorithm>
#include <iostream>

namespace agents {

/**
 * @brief Внутренняя реализация AgentRegistry
 */
class AgentRegistry::Impl {
public:
    mutable std::mutex mutex;
    std::unordered_map<std::string, std::unique_ptr<IAgent>> agents;
    std::unordered_map<std::string, AgentInfo> info;
    std::unordered_map<std::string, bool> enabled;
    AgentContext* context = nullptr;
};

// ============================================================================

AgentRegistry::AgentRegistry() : impl_(std::make_unique<Impl>()) {}

AgentRegistry::~AgentRegistry() {
    shutdown_all();
}

bool AgentRegistry::register_agent(std::unique_ptr<IAgent> agent,
                                    bool is_builtin,
                                    const std::string& path) {
    if (!agent) {
        return false;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    std::string name = agent->name();
    
    if (impl_->agents.count(name) > 0) {
        std::cerr << "[AgentRegistry] Agent already registered: " << name << std::endl;
        return false;
    }

    // Создаём информацию об агенте
    AgentInfo info;
    info.name = name;
    info.description = agent->description();
    info.version = agent->version();
    info.capabilities = agent->capabilities();
    info.is_builtin = is_builtin;
    info.is_enabled = true;
    info.path = path;

    impl_->agents[name] = std::move(agent);
    impl_->info[name] = info;
    impl_->enabled[name] = true;

    std::cout << "[AgentRegistry] Registered agent: " << name 
              << " (builtin=" << is_builtin << ")" << std::endl;

    return true;
}

bool AgentRegistry::unregister_agent(const std::string& name) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    auto it = impl_->agents.find(name);
    if (it == impl_->agents.end()) {
        return false;
    }

    // Останавливаем агент перед удалением
    it->second->shutdown();
    
    impl_->agents.erase(it);
    impl_->info.erase(name);
    impl_->enabled.erase(name);

    std::cout << "[AgentRegistry] Unregistered agent: " << name << std::endl;
    return true;
}

IAgent* AgentRegistry::get_agent(const std::string& name) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    auto it = impl_->agents.find(name);
    if (it != impl_->agents.end()) {
        return it->second.get();
    }
    return nullptr;
}

bool AgentRegistry::has_agent(const std::string& name) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->agents.count(name) > 0;
}

AgentResult AgentRegistry::execute(const AgentRequest& request) {
    std::string agent_name = request.agent_name();
    
    IAgent* agent = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        
        // Проверяем наличие агента
        auto it = impl_->agents.find(agent_name);
        if (it == impl_->agents.end()) {
            return AgentResult::not_found("Agent not found: " + agent_name);
        }
        
        // Проверяем включён ли агент
        auto enabled_it = impl_->enabled.find(agent_name);
        if (enabled_it == impl_->enabled.end() || !enabled_it->second) {
            return AgentResult::error("Agent is disabled: " + agent_name);
        }
        
        agent = it->second.get();
    }
    
    // Проверяем готовность агента
    if (!agent->is_ready()) {
        return AgentResult::error("Agent is not ready: " + agent_name);
    }
    
    // Выполняем запрос
    return agent->execute(request);
}

AgentResult AgentRegistry::execute(const std::string& agent_name,
                                    const std::string& action,
                                    const nlohmann::json& params) {
    AgentRequest request(agent_name, action, params);
    return execute(request);
}

std::vector<AgentInfo> AgentRegistry::list_agents() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    std::vector<AgentInfo> result;
    result.reserve(impl_->info.size());
    
    for (const auto& pair : impl_->info) {
        AgentInfo info = pair.second;
        info.is_enabled = impl_->enabled.at(pair.first);
        result.push_back(info);
    }
    
    // Сортируем по имени
    std::sort(result.begin(), result.end(),
              [](const AgentInfo& a, const AgentInfo& b) {
                  return a.name < b.name;
              });
    
    return result;
}

const AgentInfo* AgentRegistry::get_agent_info(const std::string& name) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    auto it = impl_->info.find(name);
    if (it != impl_->info.end()) {
        return &it->second;
    }
    return nullptr;
}

std::vector<AgentInfo> AgentRegistry::find_by_capability(
        AgentCapability capability) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    std::vector<AgentInfo> result;
    
    for (const auto& pair : impl_->info) {
        if (has_capability(pair.second.capabilities, capability)) {
            result.push_back(pair.second);
        }
    }
    
    return result;
}

bool AgentRegistry::set_agent_enabled(const std::string& name, bool enabled) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    if (impl_->agents.find(name) == impl_->agents.end()) {
        return false;
    }
    
    impl_->enabled[name] = enabled;
    
    std::cout << "[AgentRegistry] Agent '" << name << "' " 
              << (enabled ? "enabled" : "disabled") << std::endl;
    
    return true;
}

bool AgentRegistry::is_agent_enabled(const std::string& name) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    auto it = impl_->enabled.find(name);
    if (it != impl_->enabled.end()) {
        return it->second;
    }
    return false;
}

bool AgentRegistry::initialize_all(AgentContext* context) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    bool all_success = true;
    
    for (auto& pair : impl_->agents) {
        const std::string& name = pair.first;
        
        // Пропускаем отключенные агенты
        auto enabled_it = impl_->enabled.find(name);
        if (enabled_it == impl_->enabled.end() || !enabled_it->second) {
            continue;
        }
        
        IAgent* agent = pair.second.get();
        if (!agent->initialize(context)) {
            std::cerr << "[AgentRegistry] Failed to initialize agent: " 
                      << name << std::endl;
            all_success = false;
        }
    }
    
    return all_success;
}

void AgentRegistry::shutdown_all() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    for (auto& pair : impl_->agents) {
        try {
            pair.second->shutdown();
        } catch (const std::exception& e) {
            std::cerr << "[AgentRegistry] Error shutting down agent '" 
                      << pair.first << "': " << e.what() << std::endl;
        }
    }
}

AgentContext* AgentRegistry::get_context() const {
    return impl_->context;
}

void AgentRegistry::set_context(AgentContext* context) {
    impl_->context = context;
}

} // namespace agents
