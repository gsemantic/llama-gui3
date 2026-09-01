#pragma once

/**
 * @file question_agent.h
 * @brief User question agent
 *
 * Allows agents to pause and ask the user a question.
 * Uses a callback to display the question in the UI and wait for a response.
 */

#include <agents/agents.h>
#include <string>
#include <functional>

namespace agents {

/**
 * @brief Callback for asking user a question
 *
 * Takes a question string, returns the user's answer.
 * Returns empty string if user cancelled.
 */
using QuestionCallback = std::function<std::string(const std::string& question)>;

class QuestionAgent : public IAgent {
public:
    QuestionAgent();
    ~QuestionAgent() override;

    const char* name() const override;
    const char* description() const override;
    const char* version() const override;

    bool initialize(AgentContext* context) override;
    AgentResult execute(const AgentRequest& request) override;
    void shutdown() override;
    AgentCapability capabilities() const override;
    bool is_ready() const override;

    void set_question_callback(QuestionCallback callback);

private:
    AgentResult handle_ask(const AgentRequest& request);

    AgentContext* context_ = nullptr;
    bool initialized_ = false;
    QuestionCallback question_callback_;
};

} // namespace agents
