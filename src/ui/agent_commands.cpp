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
    } else if (cmd == "edit") {
        return handle_edit_command(args);
    } else if (cmd == "glob") {
        return handle_glob_command(args);
    } else if (cmd == "grep") {
        return handle_grep_command(args);
    } else if (cmd == "todo") {
        return handle_todo_command(args);
    } else if (cmd == "question") {
        return handle_question_command(args);
    } else if (cmd == "terminal") {
        return handle_terminal_command(args);
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

AgentCommandResult AgentCommands::handle_edit_command(
        const std::vector<std::string>& args) {
    
    AgentCommandResult result;
    
    if (args.size() < 3) {
        result.success = false;
        result.message = "Usage: /edit <file_path> <old_text> <new_text>";
        return result;
    }
    
    std::string file_path = args[0];
    std::string old_text = args[1];
    std::string new_text = args[2];
    
    if (!is_agent_available("edit_agent")) {
        result.success = false;
        result.message = "Edit agent not available";
        return result;
    }
    
    agents::AgentRequest request("edit_agent", "edit");
    request.with_param("file_path", file_path);
    request.with_param("old_text", old_text);
    request.with_param("new_text", new_text);
    
    auto agent_result = registry_->execute(request);
    
    result.agent_name = "edit_agent";
    result.action = "edit";
    
    if (agent_result.is_ok()) {
        result.success = true;
        result.data = agent_result.data();
        result.message = "Edit completed";
    } else {
        result.success = false;
        result.message = agent_result.message();
    }
    
    return result;
}

AgentCommandResult AgentCommands::handle_glob_command(
        const std::vector<std::string>& args) {
    
    AgentCommandResult result;
    
    if (args.empty()) {
        result.success = false;
        result.message = "Usage: /glob <pattern> [path]";
        return result;
    }
    
    std::string pattern = args[0];
    std::string path = args.size() > 1 ? args[1] : ".";
    
    if (!is_agent_available("glob_agent")) {
        result.success = false;
        result.message = "Glob agent not available";
        return result;
    }
    
    agents::AgentRequest request("glob_agent", "glob");
    request.with_param("pattern", pattern);
    request.with_param("path", path);
    
    auto agent_result = registry_->execute(request);
    
    result.agent_name = "glob_agent";
    result.action = "glob";
    
    if (agent_result.is_ok()) {
        result.success = true;
        result.data = agent_result.data();
        auto count = agent_result.get<int>("count", 0);
        result.message = "Found " + std::to_string(count) + " files";
    } else {
        result.success = false;
        result.message = agent_result.message();
    }
    
    return result;
}

AgentCommandResult AgentCommands::handle_grep_command(
        const std::vector<std::string>& args) {
    
    AgentCommandResult result;
    
    if (args.empty()) {
        result.success = false;
        result.message = "Usage: /grep <pattern> [path] [include]";
        return result;
    }
    
    std::string pattern = args[0];
    std::string path = args.size() > 1 ? args[1] : ".";
    std::string include = args.size() > 2 ? args[2] : "";
    
    if (!is_agent_available("grep_agent")) {
        result.success = false;
        result.message = "Grep agent not available";
        return result;
    }
    
    agents::AgentRequest request("grep_agent", "grep");
    request.with_param("pattern", pattern);
    request.with_param("path", path);
    if (!include.empty()) request.with_param("include", include);
    
    auto agent_result = registry_->execute(request);
    
    result.agent_name = "grep_agent";
    result.action = "grep";
    
    if (agent_result.is_ok()) {
        result.success = true;
        result.data = agent_result.data();
        auto total = agent_result.get<int>("total_matches", 0);
        result.message = "Found " + std::to_string(total) + " matches";
    } else {
        result.success = false;
        result.message = agent_result.message();
    }
    
    return result;
}

AgentCommandResult AgentCommands::handle_todo_command(
        const std::vector<std::string>& args) {
    
    AgentCommandResult result;
    result.agent_name = "todowrite_agent";
    
    if (!is_agent_available("todowrite_agent")) {
        result.success = false;
        result.message = "todowrite_agent is not available";
        return result;
    }
    
    if (args.empty()) {
        result.action = "get";
        agents::AgentRequest req("todowrite_agent", "get");
        auto agent_result = registry_->execute(req);
        
        if (agent_result.is_ok()) {
            result.success = true;
            result.data = agent_result.data();
            result.message = agent_result.message();
        } else {
            result.success = false;
            result.message = agent_result.message();
        }
        return result;
    }
    
    std::string subcmd = args[0];
    
    if (subcmd == "set" && args.size() > 1) {
        result.action = "set";
        nlohmann::json items = nlohmann::json::array();
        for (size_t i = 1; i < args.size(); ++i) {
            items.push_back({{"content", args[i]}, {"status", "pending"}, {"priority", 0}});
        }
        agents::AgentRequest req("todowrite_agent", "set", {{"items", items}});
        auto agent_result = registry_->execute(req);
        
        if (agent_result.is_ok()) {
            result.success = true;
            result.data = agent_result.data();
            result.message = agent_result.message();
        } else {
            result.success = false;
            result.message = agent_result.message();
        }
    } else if (subcmd == "get") {
        result.action = "get";
        agents::AgentRequest req("todowrite_agent", "get");
        auto agent_result = registry_->execute(req);
        
        if (agent_result.is_ok()) {
            result.success = true;
            result.data = agent_result.data();
            result.message = agent_result.message();
        } else {
            result.success = false;
            result.message = agent_result.message();
        }
    } else if (subcmd == "update" && args.size() >= 3) {
        result.action = "update";
        int index = std::stoi(args[1]);
        std::string status = args[2];
        agents::AgentRequest req("todowrite_agent", "update", {
            {"index", index}, {"status", status}
        });
        auto agent_result = registry_->execute(req);
        
        if (agent_result.is_ok()) {
            result.success = true;
            result.data = agent_result.data();
            result.message = agent_result.message();
        } else {
            result.success = false;
            result.message = agent_result.message();
        }
    } else if (subcmd == "clear") {
        result.action = "clear";
        agents::AgentRequest req("todowrite_agent", "clear");
        auto agent_result = registry_->execute(req);
        
        if (agent_result.is_ok()) {
            result.success = true;
            result.data = agent_result.data();
            result.message = agent_result.message();
        } else {
            result.success = false;
            result.message = agent_result.message();
        }
    } else {
        result.success = false;
        result.message = "Usage: /todo [set|get|update|clear] [args]\n"
                        "  /todo set item1 item2 ...  - Set new list\n"
                        "  /todo get                  - Show list\n"
                        "  /todo update <index> <status> - Update item\n"
                        "  /todo clear                - Clear list";
    }
    
    return result;
}

AgentCommandResult AgentCommands::handle_question_command(
        const std::vector<std::string>& args) {

    AgentCommandResult result;
    result.agent_name = "question_agent";

    if (!is_agent_available("question_agent")) {
        result.success = false;
        result.message = "question_agent is not available";
        return result;
    }

    if (args.empty()) {
        result.success = false;
        result.message = "Usage: /question <текст вопроса>";
        return result;
    }

    std::string question;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) question += " ";
        question += args[i];
    }

    agents::AgentRequest req("question_agent", "ask", {{"question", question}});
    auto agent_result = registry_->execute(req);

    result.action = "ask";
    if (agent_result.is_ok()) {
        result.success = true;
        result.data = agent_result.data();
        result.message = agent_result.get<std::string>("answer", "");
    } else {
        result.success = false;
        result.message = agent_result.message();
    }

    return result;
}

AgentCommandResult AgentCommands::handle_terminal_command(
        const std::vector<std::string>& args) {

    AgentCommandResult result;
    result.agent_name = "terminal_agent";

    if (!is_agent_available("terminal_agent")) {
        result.success = false;
        result.message = "terminal_agent is not available";
        return result;
    }

    if (args.empty()) {
        result.success = false;
        result.message = "Usage: /terminal <команда>";
        return result;
    }

    std::string command;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) command += " ";
        command += args[i];
    }

    agents::AgentRequest req("terminal_agent", "execute", {{"command", command}});
    auto agent_result = registry_->execute(req);

    result.action = "execute";
    if (agent_result.is_ok()) {
        result.success = true;
        result.data = agent_result.data();
        result.message = agent_result.message();
    } else {
        result.success = false;
        result.message = agent_result.message();
    }

    return result;
}

// ============================================================================
// Helper functions
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
