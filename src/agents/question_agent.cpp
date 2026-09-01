#include "agents/question_agent.h"
#include "agents/agent_context.h"
#include "agents/agent_registry.h"

namespace agents {

QuestionAgent::QuestionAgent() = default;
QuestionAgent::~QuestionAgent() = default;

const char* QuestionAgent::name() const { return "question_agent"; }
const char* QuestionAgent::description() const {
    return "Ask user a question and wait for response";
}
const char* QuestionAgent::version() const { return "1.0.0"; }

bool QuestionAgent::initialize(AgentContext* context) {
    context_ = context;
    initialized_ = true;
    return true;
}

void QuestionAgent::shutdown() {
    initialized_ = false;
    context_ = nullptr;
}

AgentCapability QuestionAgent::capabilities() const {
    return AgentCapability::CALL_OTHER_AGENTS;
}

bool QuestionAgent::is_ready() const {
    return initialized_;
}

void QuestionAgent::set_question_callback(QuestionCallback callback) {
    question_callback_ = std::move(callback);
}

AgentResult QuestionAgent::execute(const AgentRequest& request) {
    if (!initialized_) {
        return AgentResult::error("Agent not initialized");
    }

    std::string act = request.action();
    if (act.empty()) act = "ask";

    if (act == "ask") return handle_ask(request);

    return AgentResult::error("Unknown action: " + act);
}

AgentResult QuestionAgent::handle_ask(const AgentRequest& request) {
    std::string question = request.get_param<std::string>("question", "");

    if (question.empty()) {
        return AgentResult::error("No question provided");
    }

    if (!question_callback_) {
        return AgentResult::error("No question callback set. Cannot ask user.");
    }

    std::string answer = question_callback_(question);

    if (answer.empty()) {
        return AgentResult(AgentStatus::CANCELLED, "User cancelled the question")
                .with("cancelled", true);
    }

    return AgentResult(AgentStatus::OK, "Answer received")
            .with("question", question)
            .with("answer", answer);
}

} // namespace agents
