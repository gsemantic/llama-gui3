#include "agents/todowrite_agent.h"
#include "agents/agent_context.h"
#include "agents/agent_registry.h"
#include <sstream>
#include <algorithm>

namespace agents {

TodoWriteAgent::TodoWriteAgent() = default;
TodoWriteAgent::~TodoWriteAgent() = default;

const char* TodoWriteAgent::name() const { return "todowrite_agent"; }
const char* TodoWriteAgent::description() const {
    return "Task list tracking for multi-step operations";
}
const char* TodoWriteAgent::version() const { return "1.0.0"; }

bool TodoWriteAgent::initialize(AgentContext* context) {
    context_ = context;
    initialized_ = true;
    return true;
}

void TodoWriteAgent::shutdown() {
    initialized_ = false;
    context_ = nullptr;
}

AgentCapability TodoWriteAgent::capabilities() const {
    return AgentCapability::CALL_OTHER_AGENTS;
}

bool TodoWriteAgent::is_ready() const {
    return initialized_;
}

AgentResult TodoWriteAgent::execute(const AgentRequest& request) {
    if (!initialized_) {
        return AgentResult::error("Agent not initialized");
    }

    std::string act = request.action();
    if (act.empty()) act = "set";

    if (act == "set") return handle_set(request);
    if (act == "get") return handle_get(request);
    if (act == "update") return handle_update(request);
    if (act == "clear") return handle_clear(request);

    return AgentResult::error("Unknown action: " + act);
}

AgentResult TodoWriteAgent::handle_set(const AgentRequest& request) {
    auto items = request.get_param<nlohmann::json>("items", nlohmann::json::array());

    if (items.empty()) {
        return AgentResult::error("No items provided. Pass 'items' as JSON array.");
    }

    todos_.clear();
    for (const auto& item : items) {
        TodoItem todo;
        todo.content = item.value("content", "");
        todo.status = item.value("status", "pending");
        todo.priority = item.value("priority", 0);
        if (!todo.content.empty()) {
            todos_.push_back(todo);
        }
    }

    return AgentResult(AgentStatus::OK,
            "Todo list set with " + std::to_string(todos_.size()) + " items")
            .with("count", static_cast<int>(todos_.size()));
}

AgentResult TodoWriteAgent::handle_get(const AgentRequest& /*request*/) {
    if (todos_.empty()) {
        return AgentResult(AgentStatus::OK, "Todo list is empty")
                .with("count", 0)
                .with("items", nlohmann::json::array());
    }

    nlohmann::json items = nlohmann::json::array();
    for (size_t i = 0; i < todos_.size(); ++i) {
        items.push_back({
            {"index", static_cast<int>(i)},
            {"content", todos_[i].content},
            {"status", todos_[i].status},
            {"priority", todos_[i].priority}
        });
    }

    int done = std::count_if(todos_.begin(), todos_.end(),
        [](const TodoItem& t) { return t.status == "completed"; });

    std::ostringstream summary;
    summary << "Todo list: " << done << "/" << todos_.size() << " completed";

    return AgentResult(AgentStatus::OK, summary.str())
            .with("count", static_cast<int>(todos_.size()))
            .with("completed", done)
            .with("items", items);
}

AgentResult TodoWriteAgent::handle_update(const AgentRequest& request) {
    int index = request.get_param<int>("index", -1);
    std::string status = request.get_param<std::string>("status", "");
    std::string content = request.get_param<std::string>("content", "");

    if (index < 0 || index >= static_cast<int>(todos_.size())) {
        return AgentResult::error("Invalid index: " + std::to_string(index));
    }

    if (!status.empty()) {
        todos_[index].status = status;
    }
    if (!content.empty()) {
        todos_[index].content = content;
    }

    return AgentResult(AgentStatus::OK, "Item updated")
            .with("index", index)
            .with("content", todos_[index].content)
            .with("status", todos_[index].status);
}

AgentResult TodoWriteAgent::handle_clear(const AgentRequest& /*request*/) {
    size_t count = todos_.size();
    todos_.clear();
    return AgentResult(AgentStatus::OK, "Cleared " + std::to_string(count) + " items")
            .with("count", static_cast<int>(count));
}

} // namespace agents
