#include "tools_registry.h"
#include <sstream>

namespace coder {

ToolsRegistry& ToolsRegistry::instance() {
    static ToolsRegistry reg;
    return reg;
}

void ToolsRegistry::register_tool(const std::string& name, ToolHandler handler,
                                   const std::string& description) {
    tools_[name] = {name, description, std::move(handler)};
}

void ToolsRegistry::register_tools(const std::vector<ToolInfo>& tools) {
    for (const auto& t : tools) {
        tools_[t.name] = t;
    }
}

std::string ToolsRegistry::run(const std::string& tool_name, const ToolArgs& args) {
    auto it = tools_.find(tool_name);
    if (it == tools_.end()) {
        return "[ошибка] неизвестный инструмент: " + tool_name;
    }
    return it->second.handler(args);
}

bool ToolsRegistry::has(const std::string& tool_name) const {
    return tools_.find(tool_name) != tools_.end();
}

std::vector<std::string> ToolsRegistry::list_tools() const {
    std::vector<std::string> names;
    names.reserve(tools_.size());
    for (const auto& [name, info] : tools_) {
        names.push_back(name);
    }
    return names;
}

void ToolsRegistry::clear() {
    tools_.clear();
}

} // namespace coder
