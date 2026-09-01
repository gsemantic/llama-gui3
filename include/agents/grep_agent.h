#pragma once

/**
 * @file grep_agent.h
 * @brief Content search agent
 *
 * Searches file contents using regex patterns.
 */

#include <agents/agents.h>
#include <string>

namespace agents {

class GrepAgent : public IAgent {
public:
    GrepAgent();
    ~GrepAgent() override;

    const char* name() const override;
    const char* description() const override;
    const char* version() const override;

    bool initialize(AgentContext* context) override;
    AgentResult execute(const AgentRequest& request) override;
    void shutdown() override;
    AgentCapability capabilities() const override;
    bool is_ready() const override;

private:
    AgentResult handle_grep(const AgentRequest& request);

    void search_file(const std::string& file_path, const std::string& pattern,
                     nlohmann::json& results) const;

    AgentContext* context_ = nullptr;
    bool initialized_ = false;
};

} // namespace agents
