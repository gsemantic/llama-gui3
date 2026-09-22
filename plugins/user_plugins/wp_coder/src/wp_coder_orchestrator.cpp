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
    
    // Создаем и инициализируем субагент
    auto theme_agent = std::make_unique<wp_coder::WPThemeAgent>();
    if (!theme_agent->initialize(context_)) {
        return agents::AgentResult::error("Failed to initialize wp_theme_agent");
    }
    
    // Делегируем запрос
    return theme_agent->execute(request);
}

agents::AgentResult WPCoderOrchestrator::handle_generate_plugin(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Delegating to wp_plugin_agent");
    }
    
    // Создаем и инициализируем субагент
    auto plugin_agent = std::make_unique<wp_coder::WPPluginAgent>();
    if (!plugin_agent->initialize(context_)) {
        return agents::AgentResult::error("Failed to initialize wp_plugin_agent");
    }
    
    // Делегируем запрос
    return plugin_agent->execute(request);
}

agents::AgentResult WPCoderOrchestrator::handle_find_hooks(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Delegating to wp_hook_agent");
    }
    
    // Создаем и инициализируем субагент
    auto hook_agent = std::make_unique<wp_coder::WPHookAgent>();
    if (!hook_agent->initialize(context_)) {
        return agents::AgentResult::error("Failed to initialize wp_hook_agent");
    }
    
    // Делегируем запрос
    return hook_agent->execute(request);
}

agents::AgentResult WPCoderOrchestrator::handle_deploy(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Delegating to wp_deploy_agent");
    }
    
    // Создаем и инициализируем субагент
    auto deploy_agent = std::make_unique<wp_coder::WPDeployAgent>();
    if (!deploy_agent->initialize(context_)) {
        return agents::AgentResult::error("Failed to initialize wp_deploy_agent");
    }
    
    // Делегируем запрос
    return deploy_agent->execute(request);
}

agents::AgentResult WPCoderOrchestrator::handle_search_code(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Delegating to wp_rag_agent");
    }
    
    // Создаем и инициализируем субагент
    auto rag_agent = std::make_unique<wp_coder::WPRagAgent>();
    if (!rag_agent->initialize(context_)) {
        return agents::AgentResult::error("Failed to initialize wp_rag_agent");
    }
    
    // Делегируем запрос
    return rag_agent->execute(request);
}

agents::AgentResult WPCoderOrchestrator::handle_exec_cli(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Delegating to wp_terminal_agent");
    }
    
    // Создаем и инициализируем субагент
    auto terminal_agent = std::make_unique<wp_coder::WPTerminalAgent>();
    if (!terminal_agent->initialize(context_)) {
        return agents::AgentResult::error("Failed to initialize wp_terminal_agent");
    }
    
    // Делегируем запрос
    return terminal_agent->execute(request);
}

agents::AgentResult WPCoderOrchestrator::handle_file_ops(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Delegating to wp_file_agent");
    }
    
    // Создаем и инициализируем субагент
    auto file_agent = std::make_unique<wp_coder::WPFileAgent>();
    if (!file_agent->initialize(context_)) {
        return agents::AgentResult::error("Failed to initialize wp_file_agent");
    }
    
    // Делегируем запрос
    return file_agent->execute(request);
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