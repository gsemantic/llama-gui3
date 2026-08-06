/**
 * @file terminal_agent.cpp
 * @brief Реализация агента для выполнения команд терминала с песочницей
 */

#include "terminal_agent.h"
#include <agents/sandbox.h>
#include <sstream>
#include <cstdio>
#include <future>
#include <chrono>
#include <algorithm>

namespace agents {

// ============================================================================
// TerminalAgent implementation
// ============================================================================

TerminalAgent::TerminalAgent() {
    // Команды по умолчанию в белом списке
    allowed_commands_ = {
        "ls", "pwd", "whoami", "date", "echo",
        "cat", "head", "tail", "wc", "grep",
        "find", "du", "df", "uname", "hostname"
    };
}

TerminalAgent::~TerminalAgent() {
    shutdown();
}

const char* TerminalAgent::name() const {
    return "terminal_agent";
}

const char* TerminalAgent::description() const {
    return "Terminal command execution agent with sandbox protection. "
           "Supports whitelist-based command filtering and timeout control. "
           "Use with caution - executes system commands.";
}

const char* TerminalAgent::version() const {
    return "1.0.0";
}

bool TerminalAgent::initialize(AgentContext* context) {
    if (!context) {
        return false;
    }
    
    context_ = context;
    initialized_ = true;
    
    // Загрузка настроек
    auto config = context_->get_agent_config(name());
    if (config.contains("allowed_commands")) {
        allowed_commands_.clear();
        for (const auto& cmd : config["allowed_commands"]) {
            allowed_commands_.insert(cmd.get<std::string>());
        }
    }
    if (config.contains("shell")) {
        default_shell_ = config["shell"].get<std::string>();
    }
    if (config.contains("timeout_ms")) {
        default_timeout_ms_ = config["timeout_ms"].get<int>();
    }
    if (config.contains("strict_mode")) {
        strict_mode_ = config["strict_mode"].get<bool>();
    }
    
    context_->info(name(), "Initialized with " + 
                   std::to_string(allowed_commands_.size()) + " allowed commands");
    
    return true;
}

AgentResult TerminalAgent::execute(const AgentRequest& request) {
    if (!initialized_) {
        return AgentResult::error("Agent not initialized");
    }
    
    std::string action = request.action();
    
    if (action == "exec") {
        return handle_exec(request);
    } else if (action == "exec_safe") {
        return handle_exec_safe(request);
    } else if (action == "list_commands") {
        return handle_list_commands(request);
    } else if (action == "add_command") {
        return handle_add_command(request);
    } else if (action == "remove_command") {
        return handle_remove_command(request);
    }
    
    return AgentResult::error("Unknown action: " + action);
}

void TerminalAgent::shutdown() {
    if (context_) {
        context_->info(name(), "Shutting down. Executed: " + 
                       std::to_string(execution_count_) + 
                       ", Blocked: " + std::to_string(blocked_count_));
    }
    initialized_ = false;
    context_ = nullptr;
}

AgentCapability TerminalAgent::capabilities() const {
    return AgentCapability::TERMINAL_EXEC;
}

bool TerminalAgent::is_ready() const {
    return initialized_;
}

// ============================================================================
// Обработчики действий
// ============================================================================

AgentResult TerminalAgent::handle_exec(const AgentRequest& request) {
    std::string command = request.get_param<std::string>("command", "");
    int timeout = request.get_param<int>("timeout_ms", default_timeout_ms_);
    bool skip_whitelist = request.get_param<bool>("skip_whitelist", false);
    
    if (command.empty()) {
        return AgentResult::error("Command is empty");
    }
    
    // Проверка на опасные команды
    if (is_dangerous_command(command)) {
        blocked_count_++;
        if (context_) {
            context_->warning(name(), "Blocked dangerous command: " + command);
        }
        return AgentResult::error("Dangerous command blocked");
    }
    
    // Проверка белого списка (если не пропущено и не строгий режим)
    if (!skip_whitelist && strict_mode_) {
        if (!is_command_allowed(command)) {
            blocked_count_++;
            if (context_) {
                context_->warning(name(), "Command not in whitelist: " + command);
            }
            return AgentResult::error("Command not in whitelist");
        }
    }
    
    // Проверка через SecurityManager
    if (context_) {
        // auto security_result = context_->security()->check_command(name(), command);
        // if (!security_result.allowed) {
        //     return AgentResult::error(security_result.reason);
        // }
    }
    
    execution_count_++;
    
    return execute_command(command, timeout);
}

AgentResult TerminalAgent::handle_exec_safe(const AgentRequest& request) {
    std::string command = request.get_param<std::string>("command", "");
    int timeout = request.get_param<int>("timeout_ms", default_timeout_ms_);
    
    if (command.empty()) {
        return AgentResult::error("Command is empty");
    }
    
    // Строгая проверка белого списка
    if (!is_command_allowed(command)) {
        blocked_count_++;
        return AgentResult::error("Command not in whitelist");
    }
    
    execution_count_++;
    
    return execute_command(command, timeout);
}

AgentResult TerminalAgent::handle_list_commands(const AgentRequest& request) {
    (void)request;
    
    nlohmann::json commands = nlohmann::json::array();
    for (const auto& cmd : allowed_commands_) {
        commands.push_back(cmd);
    }
    
    return AgentResult::success({
        {"commands", commands},
        {"count", static_cast<int>(allowed_commands_.size())},
        {"strict_mode", strict_mode_}
    });
}

AgentResult TerminalAgent::handle_add_command(const AgentRequest& request) {
    std::string command = request.get_param<std::string>("command", "");
    
    if (command.empty()) {
        return AgentResult::error("Command is empty");
    }
    
    // Проверка на опасные команды
    if (is_dangerous_command(command)) {
        return AgentResult::error("Cannot add dangerous command to whitelist");
    }
    
    allowed_commands_.insert(command);
    
    if (context_) {
        context_->info(name(), "Added command to whitelist: " + command);
    }
    
    return AgentResult::success({
        {"command", command},
        {"added", true}
    });
}

AgentResult TerminalAgent::handle_remove_command(const AgentRequest& request) {
    std::string command = request.get_param<std::string>("command", "");
    
    if (command.empty()) {
        return AgentResult::error("Command is empty");
    }
    
    auto it = allowed_commands_.find(command);
    if (it == allowed_commands_.end()) {
        return AgentResult::error("Command not in whitelist");
    }
    
    allowed_commands_.erase(it);
    
    if (context_) {
        context_->info(name(), "Removed command from whitelist: " + command);
    }
    
    return AgentResult::success({
        {"command", command},
        {"removed", true}
    });
}

// ============================================================================
// Вспомогательные функции
// ============================================================================

bool TerminalAgent::is_command_allowed(const std::string& command) const {
    std::string base_cmd = extract_base_command(command);
    return allowed_commands_.count(base_cmd) > 0;
}

std::string TerminalAgent::extract_base_command(const std::string& command) const {
    // Извлекаем первое слово (базовую команду)
    size_t space_pos = command.find(' ');
    if (space_pos == std::string::npos) {
        return command;
    }
    return command.substr(0, space_pos);
}

bool TerminalAgent::is_dangerous_command(const std::string& command) const {
    // Опасные команды и паттерны
    static const std::vector<std::string> dangerous_patterns = {
        "rm -rf /",
        "rm -rf /*",
        "mkfs",
        "dd if=/dev/zero",
        ":(){:|:&};:",  // fork bomb
        "chmod -R 777 /",
        "wget",
        "curl",
        "> /dev/sda",
        "sudo rm",
        "su -",
        "passwd",
        "visudo",
        "iptables -F",
        "shutdown",
        "reboot",
        "poweroff",
    };
    
    std::string cmd_lower = command;
    std::transform(cmd_lower.begin(), cmd_lower.end(), cmd_lower.begin(), ::tolower);
    
    for (const auto& pattern : dangerous_patterns) {
        if (cmd_lower.find(pattern) != std::string::npos) {
            return true;
        }
    }
    
    // Проверка на перенаправление в системные файлы
    if (command.find("> /etc/") != std::string::npos ||
        command.find("> /dev/") != std::string::npos ||
        command.find(">> /etc/") != std::string::npos) {
        return true;
    }
    
    return false;
}

AgentResult TerminalAgent::execute_command(const std::string& command, 
                                            int timeout_ms) {
    if (context_) {
        context_->debug(name(), "Executing: " + command);
    }
    
    // Используем Sandbox для выполнения с таймаутом
    SandboxConfig config;
    config.timeout_ms = timeout_ms;
    
    Sandbox sandbox(config);
    
    auto result = sandbox.run_with_timeout([this, command]() {
        std::string output;
        std::string error;
        
        // Выполнение команды через popen
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe) {
            return -1;
        }
        
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            output += buffer;
        }
        
        int status = pclose(pipe);
        
        if (context_) {
            context_->debug(name(), "Command output: " + 
                           std::to_string(output.size()) + " bytes");
        }
        
        return status;
    }, timeout_ms);
    
    if (result.status == SandboxStatus::TIMEOUT) {
        return AgentResult::error("Command execution timeout");
    }
    
    if (result.status == SandboxStatus::ERROR) {
        return AgentResult::error(result.error_output);
    }
    
    nlohmann::json response;
    response["command"] = command;
    response["exit_code"] = result.exit_code;
    response["output"] = result.output;
    response["execution_time_ms"] = result.execution_time_ms;
    response["success"] = (result.exit_code == 0);
    
    return AgentResult::success(response);
}

} // namespace agents

// ============================================================================
// C-API экспорт
// ============================================================================

extern "C" {

AGENT_PLUGIN_EXPORT const char* plugin_get_name() {
    return "terminal_agent";
}

AGENT_PLUGIN_EXPORT const char* plugin_get_version() {
    return "1.0.0";
}

AGENT_PLUGIN_EXPORT const char* plugin_get_api_version() {
    return AGENT_PLUGIN_API_VERSION;
}

AGENT_PLUGIN_EXPORT agents::IAgent* plugin_create_agent() {
    return new agents::TerminalAgent();
}

AGENT_PLUGIN_EXPORT void plugin_destroy_agent(agents::IAgent* agent) {
    delete agent;
}

AGENT_PLUGIN_EXPORT PluginExports* plugin_get_exports() {
    static PluginExports exports = {
        AGENT_PLUGIN_API_VERSION,
        plugin_get_name,
        plugin_get_version,
        plugin_get_api_version,
        reinterpret_cast<plugin_create_agent_fn>(plugin_create_agent),
        reinterpret_cast<plugin_destroy_agent_fn>(plugin_destroy_agent),
        nullptr,
        nullptr
    };
    return &exports;
}

} // extern "C"
