#include "wp_coder_orchestrator.h"
#include <fstream>
#include <sstream>
#include <iomanip>

namespace wp_coder {

// ===========================================================================
// WPCoderOrchestrator implementation
// ===========================================================================

WPCoderOrchestrator::WPCoderOrchestrator() = default;

WPCoderOrchestrator::~WPCoderOrchestrator() {
    shutdown();
}

const char* WPCoderOrchestrator::name() const {
    return "wp_coder_orchestrator";
}

const char* WPCoderOrchestrator::description() const {
    return "WordPress Coder orchestrator. Delegates tasks to specialized sub-agents: theme, plugin, hooks, deploy, RAG search, WP-CLI, file operations.";
}

const char* WPCoderOrchestrator::version() const {
    return "0.1.0";
}

bool WPCoderOrchestrator::initialize(agents::AgentContext* context) {
    if (!context) {
        return false;
    }
    
    context_ = context;
    initialized_ = true;
    
    // Загрузка профиля по умолчанию
    load_harness_profile(current_profile_);
    
    if (context_) {
        context_->info(name(), "Initialized with profile: " + current_profile_);
    }
    
    return true;
}

agents::AgentResult WPCoderOrchestrator::execute(const agents::AgentRequest& request) {
    if (!initialized_) {
        return agents::AgentResult::error("Agent not initialized");
    }
    
    std::string action = request.action();
    
    if (action == "generate_theme") {
        return handle_generate_theme(request);
    } else if (action == "generate_plugin") {
        return handle_generate_plugin(request);
    } else if (action == "find_hooks") {
        return handle_find_hooks(request);
    } else if (action == "deploy") {
        return handle_deploy(request);
    } else if (action == "search_code") {
        return handle_search_code(request);
    } else if (action == "exec_cli") {
        return handle_exec_cli(request);
    } else if (action == "file_ops") {
        return handle_file_ops(request);
    }
    
    return agents::AgentResult::error("Unknown action: " + action);
}

void WPCoderOrchestrator::shutdown() {
    if (context_) {
        context_->info(name(), "Shutting down");
    }
    initialized_ = false;
    context_ = nullptr;
}

agents::AgentCapability WPCoderOrchestrator::capabilities() const {
    return agents::AgentCapability::CODE_GENERATION | 
           agents::AgentCapability::CODE_ANALYSIS |
           agents::AgentCapability::RAG_SEARCH |
           agents::AgentCapability::EXECUTION;
}

bool WPCoderOrchestrator::is_ready() const {
    return initialized_;
}

// ===========================================================================
// Обработчики действий (заглушки)
// ===========================================================================

agents::AgentResult WPCoderOrchestrator::handle_generate_theme(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Delegating to wp_theme_agent");
    }
    // TODO: Делегировать реальному wp_theme_agent
    return agents::AgentResult::success({
        {"message", "Theme generation delegated to sub-agent"},
        {"status", "delegated"}
    });
}

agents::AgentResult WPCoderOrchestrator::handle_generate_plugin(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Delegating to wp_plugin_agent");
    }
    // TODO: Делегировать реальному wp_plugin_agent
    return agents::AgentResult::success({
        {"message", "Plugin generation delegated to sub-agent"},
        {"status", "delegated"}
    });
}

agents::AgentResult WPCoderOrchestrator::handle_find_hooks(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Delegating to wp_hook_agent");
    }
    // TODO: Делегировать реальному wp_hook_agent
    return agents::AgentResult::success({
        {"message", "Hook search delegated to sub-agent"},
        {"status", "delegated"}
    });
}

agents::AgentResult WPCoderOrchestrator::handle_deploy(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Delegating to wp_deploy_agent");
    }
    // TODO: Делегировать реальному wp_deploy_agent
    return agents::AgentResult::success({
        {"message", "Deployment delegated to sub-agent"},
        {"status", "delegated"}
    });
}

agents::AgentResult WPCoderOrchestrator::handle_search_code(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Delegating to wp_rag_agent");
    }
    // TODO: Делегировать реальному wp_rag_agent
    return agents::AgentResult::success({
        {"message", "Code search delegated to sub-agent"},
        {"status", "delegated"}
    });
}

agents::AgentResult WPCoderOrchestrator::handle_exec_cli(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Delegating to wp_terminal_agent");
    }
    // TODO: Делегировать реальному wp_terminal_agent
    return agents::AgentResult::success({
        {"message", "CLI execution delegated to sub-agent"},
        {"status", "delegated"}
    });
}

agents::AgentResult WPCoderOrchestrator::handle_file_ops(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Delegating to wp_file_agent");
    }
    // TODO: Делегировать реальному wp_file_agent
    return agents::AgentResult::success({
        {"message", "File operations delegated to sub-agent"},
        {"status", "delegated"}
    });
}

// ===========================================================================
// Управление профилями
// ===========================================================================

void WPCoderOrchestrator::load_harness_profile(const std::string& profile_name) {
    std::string profile_path = "plugins/user_plugins/wp_coder/profiles/wp_coder/" + profile_name + ".json";
    
    std::ifstream file(profile_path);
    if (!file.is_open()) {
        if (context_) {
            context_->warn(name(), "Profile not found: " + profile_path + ", using defaults");
        }
        return;
    }
    
    try {
        file >> profile_config_;
        current_profile_ = profile_name;
        
        if (context_) {
            context_->info(name(), "Loaded harness profile: " + profile_name);
        }
    } catch (const std::exception& e) {
        if (context_) {
            context_->error(name(), "Failed to parse profile: " + std::string(e.what()));
        }
    }
}

std::string WPCoderOrchestrator::get_current_profile() const {
    return current_profile_;
}

} // namespace wp_coder