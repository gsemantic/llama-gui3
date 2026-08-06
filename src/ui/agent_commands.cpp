/**
 * @file agent_commands.cpp
 * @brief Реализация команд для вызова агентов
 */

#include "ui/agent_commands.h"
#include <sstream>
#include <algorithm>
#include <cctype>

namespace llama_gui {
namespace ui {

// ============================================================================
// AgentCommands implementation
// ============================================================================

AgentCommands::AgentCommands() = default;

AgentCommands::~AgentCommands() = default;

bool AgentCommands::initialize(agents::AgentRegistry* registry, 
                                agents::AgentContext* context) {
    if (!registry || !context) {
        return false;
    }
    
    registry_ = registry;
    context_ = context;
    
    return true;
}

AgentCommandResult AgentCommands::execute(const std::string& command) {
    auto args = parse_arguments(command);
    
    if (args.empty()) {
        AgentCommandResult result;
        result.success = false;
        result.message = "Empty command";
        return result;
    }
    
    std::string cmd = args[0];
    
    // Убираем ведущий '/'
    if (!cmd.empty() && cmd[0] == '/') {
        cmd = cmd.substr(1);
    }
    
    // Удаляем первый аргумент (имя команды)
    args.erase(args.begin());
    
    if (cmd == "agent") {
        return handle_agent_command(args);
    } else if (cmd == "rag") {
        return handle_rag_command(args);
    } else if (cmd == "search") {
        return handle_search_command(args);
    } else if (cmd == "summarize") {
        return handle_summarize_command(args);
    } else if (cmd == "file") {
        return handle_file_command(args);
    } else if (cmd == "code") {
        return handle_code_command(args);
    } else if (cmd == "agents") {
        return handle_agents_command(args);
    }
    
    AgentCommandResult result;
    result.success = false;
    result.message = "Unknown command: " + cmd;
    return result;
}

AgentCommandResult AgentCommands::handle_agent_command(
        const std::vector<std::string>& args) {
    
    AgentCommandResult result;
    
    if (args.size() < 2) {
        result.success = false;
        result.message = "Usage: /agent <name> <action> [params]";
        return result;
    }
    
    std::string agent_name = args[0];
    std::string action = args[1];
    
    // Проверка доступности агента
    if (!is_agent_available(agent_name)) {
        result.success = false;
        result.message = "Agent '" + agent_name + "' not found or not ready";
        result.agent_name = agent_name;
        return result;
    }
    
    // Создание запроса
    agents::AgentRequest request(agent_name, action);
    
    // Парсинг параметров
    auto params = parse_params(args);
    for (auto& [key, value] : params.items()) {
        request.with_param(key, value);
    }
    
    // Выполнение
    if (context_) {
        context_->info("AgentCommands", "Executing " + agent_name + ":" + action);
    }
    
    auto agent_result = registry_->execute(request);
    
    result.agent_name = agent_name;
    result.action = action;
    
    if (agent_result.is_ok()) {
        result.success = true;
        result.data = agent_result.data();
        result.message = "Success";
    } else {
        result.success = false;
        result.message = agent_result.message();
    }
    
    return result;
}

AgentCommandResult AgentCommands::handle_rag_command(
        const std::vector<std::string>& args) {
    
    AgentCommandResult result;
    
    if (args.empty()) {
        result.success = false;
        result.message = "Usage: /rag <query>";
        return result;
    }
    
    // Сборка запроса из всех аргументов
    std::string query;
    for (size_t i = 0; i < args.size(); i++) {
        if (i > 0) query += " ";
        query += args[i];
    }
    
    // Проверка доступности rag_agent
    if (!is_agent_available("rag_agent")) {
        result.success = false;
        result.message = "RAG agent not available";
        return result;
    }
    
    agents::AgentRequest request("rag_agent", "search");
    request.with_param("query", query);
    request.with_param("k", 5);
    
    auto agent_result = registry_->execute(request);
    
    result.agent_name = "rag_agent";
    result.action = "search";
    
    if (agent_result.is_ok()) {
        result.success = true;
        result.data = agent_result.data();
        result.message = "RAG search completed";
    } else {
        result.success = false;
        result.message = agent_result.message();
    }
    
    return result;
}

AgentCommandResult AgentCommands::handle_search_command(
        const std::vector<std::string>& args) {
    
    AgentCommandResult result;
    
    if (args.empty()) {
        result.success = false;
        result.message = "Usage: /search <query>";
        return result;
    }
    
    std::string query;
    for (size_t i = 0; i < args.size(); i++) {
        if (i > 0) query += " ";
        query += args[i];
    }
    
    if (!is_agent_available("web_search_agent")) {
        result.success = false;
        result.message = "Web Search agent not available";
        return result;
    }
    
    agents::AgentRequest request("web_search_agent", "search");
    request.with_param("query", query);
    
    auto agent_result = registry_->execute(request);
    
    result.agent_name = "web_search_agent";
    result.action = "search";
    
    if (agent_result.is_ok()) {
        result.success = true;
        result.data = agent_result.data();
        result.message = "Web search completed";
    } else {
        result.success = false;
        result.message = agent_result.message();
    }
    
    return result;
}

AgentCommandResult AgentCommands::handle_summarize_command(
        const std::vector<std::string>& args) {
    
    AgentCommandResult result;
    
    if (args.empty()) {
        result.success = false;
        result.message = "Usage: /summarize <text>";
        return result;
    }
    
    std::string text;
    for (size_t i = 0; i < args.size(); i++) {
        if (i > 0) text += " ";
        text += args[i];
    }
    
    if (!is_agent_available("summarization_agent")) {
        result.success = false;
        result.message = "Summarization agent not available";
        return result;
    }
    
    agents::AgentRequest request("summarization_agent", "summarize");
    request.with_param("text", text);
    request.with_param("max_sentences", 3);
    
    auto agent_result = registry_->execute(request);
    
    result.agent_name = "summarization_agent";
    result.action = "summarize";
    
    if (agent_result.is_ok()) {
        result.success = true;
        result.data = agent_result.data();
        result.message = "Summary: " + agent_result.get<std::string>("summary", "");
    } else {
        result.success = false;
        result.message = agent_result.message();
    }
    
    return result;
}

AgentCommandResult AgentCommands::handle_file_command(
        const std::vector<std::string>& args) {
    
    AgentCommandResult result;
    
    if (args.size() < 2) {
        result.success = false;
        result.message = "Usage: /file <action> <path>";
        return result;
    }
    
    std::string action = args[0];
    std::string path = args[1];
    
    if (!is_agent_available("file_agent")) {
        result.success = false;
        result.message = "File agent not available";
        return result;
    }
    
    agents::AgentRequest request("file_agent", action);
    request.with_param("file_path", path);
    
    auto agent_result = registry_->execute(request);
    
    result.agent_name = "file_agent";
    result.action = action;
    
    if (agent_result.is_ok()) {
        result.success = true;
        result.data = agent_result.data();
        result.message = "File operation completed";
    } else {
        result.success = false;
        result.message = agent_result.message();
    }
    
    return result;
}

AgentCommandResult AgentCommands::handle_code_command(
        const std::vector<std::string>& args) {
    
    AgentCommandResult result;
    
    if (args.size() < 2) {
        result.success = false;
        result.message = "Usage: /code <language> <prompt>";
        return result;
    }
    
    std::string language = args[0];
    
    std::string prompt;
    for (size_t i = 1; i < args.size(); i++) {
        if (i > 1) prompt += " ";
        prompt += args[i];
    }
    
    if (!is_agent_available("code_agent")) {
        result.success = false;
        result.message = "Code agent not available";
        return result;
    }
    
    agents::AgentRequest request("code_agent", "generate");
    request.with_param("language", language);
    request.with_param("prompt", prompt);
    
    auto agent_result = registry_->execute(request);
    
    result.agent_name = "code_agent";
    result.action = "generate";
    
    if (agent_result.is_ok()) {
        result.success = true;
        result.data = agent_result.data();
        result.message = "Code generated";
    } else {
        result.success = false;
        result.message = agent_result.message();
    }
    
    return result;
}

AgentCommandResult AgentCommands::handle_agents_command(
        const std::vector<std::string>& args) {
    
    AgentCommandResult result;
    
    if (!registry_) {
        result.success = false;
        result.message = "Agent registry not available";
        return result;
    }
    
    std::string subcommand = args.empty() ? "list" : args[0];
    
    if (subcommand == "list") {
        auto agent_list = registry_->list_agents();
        nlohmann::json agents_json = nlohmann::json::array();
        
        for (const auto& info : agent_list) {
            nlohmann::json agent;
            agent["name"] = info.name;
            agent["version"] = info.version;
            agent["enabled"] = registry_->is_agent_enabled(info.name);
            agent["builtin"] = info.is_builtin;
            agents_json.push_back(agent);
        }
        
        result.success = true;
        result.data["agents"] = agents_json;
        result.message = "Found " + std::to_string(agent_list.size()) + " agents";
    } else if (subcommand == "status") {
        auto agent_list = registry_->list_agents();
        int enabled = 0, disabled = 0;
        
        for (const auto& info : agent_list) {
            if (registry_->is_agent_enabled(info.name)) {
                enabled++;
            } else {
                disabled++;
            }
        }
        
        result.success = true;
        result.data["total"] = static_cast<int>(agent_list.size());
        result.data["enabled"] = enabled;
        result.data["disabled"] = disabled;
        result.message = "Total: " + std::to_string(agent_list.size()) + 
                        ", Enabled: " + std::to_string(enabled) +
                        ", Disabled: " + std::to_string(disabled);
    } else {
        result.success = false;
        result.message = "Unknown subcommand: " + subcommand + 
                        ". Use: list, status";
    }
    
    result.agent_name = "system";
    result.action = "agents " + subcommand;
    
    return result;
}

// ============================================================================
// Вспомогательные функции
// ============================================================================

std::vector<std::string> AgentCommands::parse_arguments(const std::string& command) {
    std::vector<std::string> args;
    std::istringstream iss(command);
    std::string arg;
    
    while (iss >> arg) {
        args.push_back(arg);
    }
    
    return args;
}

nlohmann::json AgentCommands::parse_params(const std::vector<std::string>& args) {
    nlohmann::json params;
    
    // Простой парсинг key=value
    for (size_t i = 2; i < args.size(); i++) {  // Пропускаем name и action
        const std::string& arg = args[i];
        auto eq_pos = arg.find('=');
        
        if (eq_pos != std::string::npos) {
            std::string key = arg.substr(0, eq_pos);
            std::string value = arg.substr(eq_pos + 1);
            params[key] = value;
        }
    }
    
    return params;
}

void AgentCommands::set_on_result(std::function<void(const AgentCommandResult&)> callback) {
    on_result_ = std::move(callback);
}

bool AgentCommands::is_agent_available(const std::string& name) const {
    if (!registry_) {
        return false;
    }
    
    if (!registry_->has_agent(name)) {
        return false;
    }
    
    if (!registry_->is_agent_enabled(name)) {
        return false;
    }
    
    auto* agent = registry_->get_agent(name);
    return agent && agent->is_ready();
}

std::string AgentCommands::format_result(const AgentCommandResult& result) const {
    std::ostringstream oss;
    
    if (result.success) {
        oss << "✓ ";
    } else {
        oss << "✗ ";
    }
    
    oss << "[" << result.agent_name << "] " << result.message;
    
    return oss.str();
}

} // namespace ui
} // namespace llama_gui
