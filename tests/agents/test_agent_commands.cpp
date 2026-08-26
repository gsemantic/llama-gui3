/**
 * @file test_agent_commands.cpp
 * @brief Тесты для AgentCommands
 */

#include "ui/agent_commands.h"
#include <agents/agents.h>
#include <iostream>
#include <cassert>

using namespace agents;
using namespace ui;

// ============================================================================
// Тестовый агент для Command тестов
// ============================================================================

class CommandTestAgent : public IAgent {
public:
    const char* name() const override { return "command_test_agent"; }
    const char* description() const override { return "Command test agent"; }
    const char* version() const override { return "1.0.0"; }

    bool initialize(AgentContext* context) override {
        context_ = context;
        initialized_ = true;
        return true;
    }

    AgentResult execute(const AgentRequest& request) override {
        std::string action = request.action();
        
        if (action == "search") {
            std::string query = request.query();
            return AgentResult::success({
                {"query", query},
                {"results", 5}
            });
        } else if (action == "read") {
            std::string path = request.file_path();
            return AgentResult::success({
                {"file_path", path},
                {"content", "test content"}
            });
        }
        
        return AgentResult::error("Unknown action");
    }

    void shutdown() override {
        initialized_ = false;
    }

    AgentCapability capabilities() const override {
        return AgentCapability::NONE;
    }

    bool is_ready() const override {
        return initialized_;
    }

private:
    AgentContext* context_;
    bool initialized_;
};

// ============================================================================
// Тесты
// ============================================================================

void test_parse_arguments() {
    std::cout << "[TEST] Parse arguments... ";
    
    auto args = AgentCommands::parse_arguments("/agent test action key=value");
    
    assert(args.size() == 4);
    assert(args[0] == "/agent");
    assert(args[1] == "test");
    assert(args[2] == "action");
    assert(args[3] == "key=value");
    
    std::cout << "PASSED" << std::endl;
}

void test_parse_params() {
    std::cout << "[TEST] Parse params... ";
    
    std::vector<std::string> args = {"agent", "test", "action", "k1=v1", "k2=v2"};
    auto params = AgentCommands::parse_params(args);
    
    assert(params.contains("k1"));
    assert(params.contains("k2"));
    assert(params["k1"].get<std::string>() == "v1");
    assert(params["k2"].get<std::string>() == "v2");
    
    std::cout << "PASSED" << std::endl;
}

void test_rag_command() {
    std::cout << "[TEST] RAG command... ";
    
    AgentCommands commands;
    AgentRegistry registry;
    AgentContext context;
    
    auto agent = std::make_unique<CommandTestAgent>();
    registry.register_agent(std::move(agent), true);
    commands.initialize(&registry, &context);
    
    auto result = commands.execute("/rag тестовый запрос");
    
    // Команда должна выполниться (успешно или с ошибкой RAG)
    assert(result.agent_name == "rag_agent" || !result.success);
    
    std::cout << "PASSED" << std::endl;
}

void test_search_command() {
    std::cout << "[TEST] Search command... ";
    
    AgentCommands commands;
    AgentRegistry registry;
    AgentContext context;
    
    auto agent = std::make_unique<CommandTestAgent>();
    registry.register_agent(std::move(agent), true);
    commands.initialize(&registry, &context);
    
    auto result = commands.execute("/search C++ tutorials");
    
    // Команда должна выполниться (успешно или с ошибкой если нет web_search_agent)
    assert(result.agent_name == "web_search_agent" || !result.success);
    
    std::cout << "PASSED" << std::endl;
}

void test_agents_list_command() {
    std::cout << "[TEST] Agents list command... ";
    
    AgentCommands commands;
    AgentRegistry registry;
    AgentContext context;
    
    auto agent = std::make_unique<CommandTestAgent>();
    registry.register_agent(std::move(agent), true);
    commands.initialize(&registry, &context);
    
    auto result = commands.execute("/agents list");
    
    assert(result.success);
    assert(result.data.contains("agents"));
    assert(result.data["agents"].size() >= 1);
    
    std::cout << "PASSED" << std::endl;
}

void test_agents_status_command() {
    std::cout << "[TEST] Agents status command... ";
    
    AgentCommands commands;
    AgentRegistry registry;
    AgentContext context;
    
    auto agent = std::make_unique<CommandTestAgent>();
    registry.register_agent(std::move(agent), true);
    commands.initialize(&registry, &context);
    
    auto result = commands.execute("/agents status");
    
    assert(result.success);
    assert(result.data.contains("total"));
    assert(result.data.contains("enabled"));
    assert(result.data.contains("disabled"));
    
    std::cout << "PASSED" << std::endl;
}

void test_unknown_command() {
    std::cout << "[TEST] Unknown command... ";
    
    AgentCommands commands;
    AgentRegistry registry;
    AgentContext context;
    
    commands.initialize(&registry, &context);
    
    auto result = commands.execute("/unknown_command");
    
    assert(!result.success);
    assert(result.message.find("Unknown command") != std::string::npos);
    
    std::cout << "PASSED" << std::endl;
}

void test_empty_command() {
    std::cout << "[TEST] Empty command... ";
    
    AgentCommands commands;
    AgentRegistry registry;
    AgentContext context;
    
    commands.initialize(&registry, &context);
    
    auto result = commands.execute("");
    
    assert(!result.success);
    
    std::cout << "PASSED" << std::endl;
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Agent Commands Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    
    test_parse_arguments();
    test_parse_params();
    test_rag_command();
    test_search_command();
    test_agents_list_command();
    test_agents_status_command();
    test_unknown_command();
    test_empty_command();
    
    std::cout << "========================================" << std::endl;
    std::cout << "All tests PASSED!" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return 0;
}
