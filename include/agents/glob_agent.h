#pragma once

#include <agents/agents.h>
#include <string>
#include <vector>

namespace agents {

class GlobAgent : public IAgent {
public:
    GlobAgent();
    ~GlobAgent() override;

    const char* name() const override;
    const char* description() const override;
    const char* version() const override;

    bool initialize(AgentContext* context) override;
    AgentResult execute(const AgentRequest& request) override;
    void shutdown() override;
    AgentCapability capabilities() const override;
    bool is_ready() const override;

private:
    AgentResult handle_glob(const AgentRequest& request);
    AgentResult handle_list(const AgentRequest& request);

    void walk_directory(const std::string& root, const std::string& pattern,
                        std::vector<std::string>& results, bool recursive) const;

    bool match_glob(const std::string& text, const std::string& pattern) const;

    AgentContext* context_ = nullptr;
    bool initialized_ = false;
};

} // namespace agents
