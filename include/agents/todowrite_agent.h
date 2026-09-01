#pragma once

/**
 * @file todowrite_agent.h
 * @brief Task list tracking agent
 *
 * Manages a simple in-memory task list that agents can use
 * to track progress during multi-step operations.
 */

#include <agents/agents.h>
#include <string>
#include <vector>

namespace agents {

struct TodoItem {
    std::string content;
    std::string status;  // "pending", "in_progress", "completed", "cancelled"
    int priority = 0;    // 0=low, 1=medium, 2=high
};

class TodoWriteAgent : public IAgent {
public:
    TodoWriteAgent();
    ~TodoWriteAgent() override;

    const char* name() const override;
    const char* description() const override;
    const char* version() const override;

    bool initialize(AgentContext* context) override;
    AgentResult execute(const AgentRequest& request) override;
    void shutdown() override;
    AgentCapability capabilities() const override;
    bool is_ready() const override;

private:
    AgentResult handle_set(const AgentRequest& request);
    AgentResult handle_get(const AgentRequest& request);
    AgentResult handle_update(const AgentRequest& request);
    AgentResult handle_clear(const AgentRequest& request);

    AgentContext* context_ = nullptr;
    bool initialized_ = false;
    std::vector<TodoItem> todos_;
};

} // namespace agents
