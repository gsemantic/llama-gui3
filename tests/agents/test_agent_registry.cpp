/**
 * @file test_agent_registry.cpp
 * @brief Тесты для AgentRegistry
 */

#include <agents/agents.h>
#include <iostream>
#include <cassert>
#include <memory>

using namespace agents;

// ============================================================================
// Тестовый агент для проверки функциональности
// ============================================================================

class TestAgent : public IAgent {
public:
    TestAgent(const std::string& name = "test_agent") 
        : name_(name), initialized_(false) {}

    const char* name() const override { return name_.c_str(); }
    const char* description() const override { return "Test agent"; }
    const char* version() const override { return "1.0.0"; }

    bool initialize(AgentContext* context) override {
        context_ = context;
        initialized_ = true;
        return true;
    }

    AgentResult execute(const AgentRequest& request) override {
        std::string action = request.action();
        
        if (action == "echo") {
            return AgentResult::success(request.params());
        } else if (action == "add") {
            int a = request.get_param<int>("a", 0);
            int b = request.get_param<int>("b", 0);
            return AgentResult::success({{"result", a + b}});
        } else if (action == "error") {
            return AgentResult::error("Intentional error");
        }
        
        return AgentResult::error("Unknown action: " + action);
    }

    void shutdown() override {
        initialized_ = false;
        context_ = nullptr;
    }

    AgentCapability capabilities() const override {
        return AgentCapability::NONE;
    }

    bool is_ready() const override {
        return initialized_;
    }

private:
    std::string name_;
    bool initialized_;
    AgentContext* context_;
};

// ============================================================================
// Тесты
// ============================================================================

void test_register_agent() {
    std::cout << "[TEST] Register agent... ";
    
    AgentRegistry registry;
    auto agent = std::make_unique<TestAgent>();
    
    bool result = registry.register_agent(std::move(agent), true);
    assert(result);
    assert(registry.has_agent("test_agent"));
    
    std::cout << "PASSED" << std::endl;
}

void test_unregister_agent() {
    std::cout << "[TEST] Unregister agent... ";
    
    AgentRegistry registry;
    auto agent = std::make_unique<TestAgent>();
    registry.register_agent(std::move(agent), true);
    
    bool result = registry.unregister_agent("test_agent");
    assert(result);
    assert(!registry.has_agent("test_agent"));
    
    std::cout << "PASSED" << std::endl;
}

void test_execute_request() {
    std::cout << "[TEST] Execute request... ";
    
    AgentRegistry registry;
    AgentContext context;
    
    auto agent = std::make_unique<TestAgent>();
    registry.register_agent(std::move(agent), true);
    registry.initialize_all(&context);
    
    AgentRequest request("test_agent", "echo");
    request.with_param("message", "hello");
    
    AgentResult result = registry.execute(request);
    assert(result.is_ok());
    assert(result.get<std::string>("message") == "hello");
    
    std::cout << "PASSED" << std::endl;
}

void test_execute_with_params() {
    std::cout << "[TEST] Execute with parameters... ";
    
    AgentRegistry registry;
    AgentContext context;
    
    auto agent = std::make_unique<TestAgent>();
    registry.register_agent(std::move(agent), true);
    registry.initialize_all(&context);
    
    AgentRequest request("test_agent", "add");
    request.with_param("a", 5);
    request.with_param("b", 3);
    
    AgentResult result = registry.execute(request);
    assert(result.is_ok());
    assert(result.get<int>("result") == 8);
    
    std::cout << "PASSED" << std::endl;
}

void test_execute_error() {
    std::cout << "[TEST] Execute error handling... ";
    
    AgentRegistry registry;
    AgentContext context;
    
    auto agent = std::make_unique<TestAgent>();
    registry.register_agent(std::move(agent), true);
    registry.initialize_all(&context);
    
    AgentRequest request("test_agent", "error");
    AgentResult result = registry.execute(request);
    
    assert(!result.is_ok());
    assert(result.message() == "Intentional error");
    
    std::cout << "PASSED" << std::endl;
}

void test_agent_not_found() {
    std::cout << "[TEST] Agent not found... ";
    
    AgentRegistry registry;
    
    AgentRequest request("nonexistent_agent", "action");
    AgentResult result = registry.execute(request);
    
    assert(!result.is_ok());
    assert(result.status() == AgentStatus::NOT_FOUND);
    
    std::cout << "PASSED" << std::endl;
}

void test_list_agents() {
    std::cout << "[TEST] List agents... ";
    
    AgentRegistry registry;
    
    auto agent1 = std::make_unique<TestAgent>("agent1");
    auto agent2 = std::make_unique<TestAgent>("agent2");
    
    registry.register_agent(std::move(agent1), true);
    registry.register_agent(std::move(agent2), false);
    
    auto agents = registry.list_agents();
    assert(agents.size() == 2);
    
    std::cout << "PASSED" << std::endl;
}

void test_enable_disable_agent() {
    std::cout << "[TEST] Enable/Disable agent... ";
    
    AgentRegistry registry;
    
    auto agent = std::make_unique<TestAgent>();
    registry.register_agent(std::move(agent), true);
    
    assert(registry.is_agent_enabled("test_agent"));
    
    registry.set_agent_enabled("test_agent", false);
    assert(!registry.is_agent_enabled("test_agent"));
    
    registry.set_agent_enabled("test_agent", true);
    assert(registry.is_agent_enabled("test_agent"));
    
    std::cout << "PASSED" << std::endl;
}

void test_find_by_capability() {
    std::cout << "[TEST] Find by capability... ";
    
    AgentRegistry registry;
    
    auto agent = std::make_unique<TestAgent>();
    registry.register_agent(std::move(agent), true);
    
    // TestAgent имеет capability NONE
    auto results = registry.find_by_capability(AgentCapability::NONE);
    assert(results.size() >= 1);
    
    std::cout << "PASSED" << std::endl;
}

void test_shutdown_all() {
    std::cout << "[TEST] Shutdown all... ";
    
    AgentRegistry registry;
    AgentContext context;
    
    auto agent1 = std::make_unique<TestAgent>("agent1");
    auto agent2 = std::make_unique<TestAgent>("agent2");
    
    registry.register_agent(std::move(agent1), true);
    registry.register_agent(std::move(agent2), true);
    registry.initialize_all(&context);
    
    registry.shutdown_all();
    
    // После shutdown агенты должны быть не готовы
    assert(!registry.get_agent("agent1")->is_ready());
    assert(!registry.get_agent("agent2")->is_ready());
    
    std::cout << "PASSED" << std::endl;
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Agent Registry Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    
    test_register_agent();
    test_unregister_agent();
    test_execute_request();
    test_execute_with_params();
    test_execute_error();
    test_agent_not_found();
    test_list_agents();
    test_enable_disable_agent();
    test_find_by_capability();
    test_shutdown_all();
    
    std::cout << "========================================" << std::endl;
    std::cout << "All tests PASSED!" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return 0;
}
