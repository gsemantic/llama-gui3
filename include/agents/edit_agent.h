#pragma once

/**
 * @file edit_agent.h
 * @brief File text replacement agent
 *
 * Performs exact string replacements in files (like opencode edit tool).
 */

#include <agents/agents.h>
#include <string>

namespace agents {

class EditAgent : public IAgent {
public:
    EditAgent();
    ~EditAgent() override;

    const char* name() const override;
    const char* description() const override;
    const char* version() const override;

    bool initialize(AgentContext* context) override;
    AgentResult execute(const AgentRequest& request) override;
    void shutdown() override;
    AgentCapability capabilities() const override;
    bool is_ready() const override;

private:
    AgentResult handle_edit(const AgentRequest& request);
    AgentResult handle_replace(const AgentRequest& request);

    AgentContext* context_ = nullptr;
    bool initialized_ = false;
};

} // namespace agents
