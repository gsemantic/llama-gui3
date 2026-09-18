#include "wp_terminal_agent.h"
#include <fstream>
#include <sstream>
#include <iomanip>

namespace wp_coder {

// ===========================================================================
// WPTerminalAgent implementation
// ===========================================================================

WPTerminalAgent::WPTerminalAgent() = default;

WPTerminalAgent::~WPTerminalAgent() {
    shutdown();
}

const char* WPTerminalAgent::name() const {
    return "wp_terminal_agent";
}

const char* WPTerminalAgent::description() const {
    return "WordPress Terminal agent. Executes WP-CLI and shell commands with whitelist.";
}

const char* WPTerminalAgent::version() const {
    return "0.1.0";
}

bool WPTerminalAgent::initialize(agents::AgentContext* context) {
    if (!context) {
        return false;
    }
    
    context_ = context;
    initialized_ = true;
    
    // Инициализация белого списка
    allowed_commands_ = {
        "wp", "ls", "cat", "mkdir", "cp", "mv", "rm", "find", "grep"
    };
    
    if (context_) {
        context_->info(name(), "Initialized with whitelist");
    }
    
    return true;
}

agents::AgentResult WPTerminalAgent::execute(const agents::AgentRequest& request) {
    if (!initialized_) {
        return agents::AgentResult::error("Agent not initialized");
    }
    
    std::string action = request.action();
    
    if (action == "exec") {
        return handle_exec(request);
    } else if (action == "exec_safe") {
        return handle_exec_safe(request);
    } else if (action == "wp_cli") {
        return handle_wp_cli(request);
    } else if (action == "list_commands") {
        return handle_list_commands(request);
    } else if (action == "add_command") {
        return handle_add_command(request);
    }
    
    return agents::AgentResult::error("Unknown action: " + action);
}

void WPTerminalAgent::shutdown() {
    if (context_) {
        context_->info(name(), "Shutting down");
    }
    initialized_ = false;
    context_ = nullptr;
}

agents::AgentCapability WPTerminalAgent::capabilities() const {
    return agents::AgentCapability::EXECUTION;
}

bool WPTerminalAgent::is_ready() const {
    return initialized_;
}

// ===========================================================================
// Обработчики действий (заглушки)
// ===========================================================================

agents::AgentResult WPTerminalAgent::handle_exec(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Executing command");
    }
    // TODO: Реальное выполнение через shell::run_capture
    return agents::AgentResult::success({
        {"message", "Command execution not implemented yet"},
        {"status", "stub"}
    });
}

agents::AgentResult WPTerminalAgent::handle_exec_safe(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Executing safe command");
    }
    // TODO: Выполнение с проверкой белого списка
    return agents::AgentResult::success({
        {"message", "Safe execution not implemented yet"},
        {"status", "stub"}
    });
}

agents::AgentResult WPTerminalAgent::handle_wp_cli(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Executing WP-CLI command");
    }
    // TODO: Выполнение WP-CLI
    return agents::AgentResult::success({
        {"message", "WP-CLI execution not implemented yet"},
        {"status", "stub"}
    });
}

agents::AgentResult WPTerminalAgent::handle_list_commands(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Listing allowed commands");
    }
    
    nlohmann::json commands = nlohmann::json::array();
    for (const auto& cmd : allowed_commands_) {
        commands.push_back(cmd);
    }
    
    return agents::AgentResult::success({
        {"commands", commands},
        {"count", static_cast<int>(allowed_commands_.size())}
    });
}

agents::AgentResult WPTerminalAgent::handle_add_command(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Adding command to whitelist");
    }
    
    std::string command = request.get_param<std::string>("command", "");
    if (command.empty()) {
        return agents::AgentResult::error("Command is empty");
    }
    
    allowed_commands_.insert(command);
    
    return agents::AgentResult::success({
        {"message", "Command added to whitelist"},
        {"command", command}
    });
}

// ===========================================================================
// Вспомогательные функции
// ===========================================================================

bool WPTerminalAgent::is_command_allowed(const std::string& command) const {
    std::string base = extract_base_command(command);
    return allowed_commands_.find(base) != allowed_commands_.end();
}

std::string WPTerminalAgent::extract_base_command(const std::string& command) const {
    std::istringstream iss(command);
    std::string base;
    iss >> base;
    return base;
}

bool WPTerminalAgent::is_dangerous_command(const std::string& command) const {
    // Простая проверка на опасные команды
    std::string dangerous[] = {"rm -rf", "dd", "mkfs", "reboot", "shutdown"};
    for (const auto& d : dangerous) {
        if (command.find(d) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace wp_coder