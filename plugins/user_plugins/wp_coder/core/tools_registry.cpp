// tools_registry.cpp — хранение ToolDef и генерация каталога из схем (И1.5).

#include "tools_registry.h"
#include "engine.h"
#include "json_utils.h"
#include "tool_protocol.h"

#include <sstream>

namespace coder {

namespace {

/* Название типа в понятном модели виде: не "integer", а "целое" —
 * описание уходит в системный промпт, и его читает модель. */
const char* type_label(const std::string& schema_type) {
    if (schema_type == "string") return "текст";
    if (schema_type == "integer") return "целое";
    if (schema_type == "number") return "число";
    if (schema_type == "boolean") return "да/нет";
    if (schema_type == "array") return "список";
    if (schema_type == "object") return "объект";
    return nullptr;
}

/* "path: текст — путь к файлу" для одного параметра. */
std::string describe_param(const std::string& name,
                           const json::JsonValue& schema) {
    std::string label = type_label(schema.get_string("type"));
    std::string desc = schema.get_string("description");
    if (schema.has("enum")) {
        std::string values;
        const json::JsonValue& list = schema.get("enum");
        for (size_t i = 0; i < list.size(); ++i) {
            if (i) values += "|";
            values += list.at(i).as_string();
        }
        desc += " (" + values + ")";
    }
    if (label.empty() && desc.empty()) return "";
    std::string s = name + ": ";
    s += label.empty() ? "значение" : label;
    if (!desc.empty()) s += " — " + desc;
    return s;
}

/* Обязательность помечаем суффиксом: "(обязательно)". */
bool is_required(const json::JsonValue& parameters, const std::string& name) {
    if (!parameters.has("required")) return false;
    const json::JsonValue& req = parameters.get("required");
    for (size_t i = 0; i < req.size(); ++i) {
        if (req.at(i).as_string() == name) return true;
    }
    return false;
}

} // anonymous namespace

ToolsRegistry& ToolsRegistry::instance() {
    static ToolsRegistry reg;
    return reg;
}

void ToolsRegistry::register_def(ToolDef def) {
    if (def.name.empty()) return;
    tools_[def.name] = std::move(def);
}

ToolOutput ToolsRegistry::run_output(const std::string& tool_name,
                                     const json::JsonValue& args) {
    ToolOutput out;
    auto it = tools_.find(tool_name);
    if (it == tools_.end()) {
        out.output = "[ошибка] неизвестный инструмент: " + tool_name;
        return out;
    }
    const ToolDef& def = it->second;

    /* Валидация по схеме (И1.6). Ошибка возвращается как текст: модель
     * читает RESULT и переписывает вызов, поэтому нужен тот же канал,
     * а не исключение. */
    std::string detail;
    if (!validate_tool_args(def.parameters, args, detail)) {
        out.output = "[ошибка] " + invalid_arguments_message(tool_name, detail);
        return out;
    }
    if (!def.handler) {
        out.output = "[ошибка] инструмент " + tool_name
                   + " зарегистрирован без обработчика";
        return out;
    }

    auto& eng = Engine::instance();
    ToolContext ctx(eng.state(), eng.callbacks());
    out = def.handler(args, ctx);
    out.output = text::sanitize_utf8(out.output);
    if (out.title.empty()) out.title = def.description;
    return out;
}

std::string ToolsRegistry::run(const std::string& tool_name,
                               const json::JsonValue& args) {
    return run_output(tool_name, args).output;
}

std::string ToolsRegistry::run(const std::string& tool_name, const ToolArgs& args) {
    Action legacy;
    legacy.path = args.path;
    legacy.root = args.root;
    legacy.query = args.query;
    legacy.pattern = args.pattern;
    legacy.content = args.content;
    legacy.cli = args.cli;
    legacy.url = args.url;
    legacy.k = args.k;
    return run(tool_name, action_to_json(legacy));
}

bool ToolsRegistry::has(const std::string& tool_name) const {
    return tools_.find(tool_name) != tools_.end();
}

const ToolDef* ToolsRegistry::find(const std::string& tool_name) const {
    auto it = tools_.find(tool_name);
    return it == tools_.end() ? nullptr : &it->second;
}

std::vector<std::string> ToolsRegistry::list_tools() const {
    std::vector<std::string> names;
    names.reserve(tools_.size());
    for (const auto& kv : tools_) names.push_back(kv.first);
    return names;
}

std::vector<ToolDef> ToolsRegistry::defs() const {
    std::vector<ToolDef> out;
    out.reserve(tools_.size());
    for (const auto& kv : tools_) out.push_back(kv.second);
    return out;
}

std::string ToolsRegistry::describe_tool(const std::string& tool_name) const {
    const ToolDef* def = find(tool_name);
    if (!def) return "";
    std::string s = "- " + def->name;
    if (!def->description.empty()) s += " — " + def->description;
    if (!def->parameters.is_object()) return s;
    const json::JsonValue& props = def->parameters.get("properties");
    if (!props.is_object()) return s;
    for (const auto& entry : props.entries()) {
        std::string line = describe_param(entry.first, *entry.second);
        if (line.empty()) continue;
        if (is_required(def->parameters, entry.first)) line += " (обязательно)";
        s += "\n    " + line;
    }
    return s;
}

std::string ToolsRegistry::build_tool_catalogue() const {
    std::string s;
    for (const auto& kv : tools_) {
        s += describe_tool(kv.first);
        s += "\n";
    }
    return s;
}

std::string ToolsRegistry::build_tool_catalogue(
        const std::vector<std::string>& visible) const {
    if (visible.empty()) return build_tool_catalogue();
    std::string s;
    for (const auto& name : visible) {
        if (tools_.find(name) == tools_.end()) continue;
        s += describe_tool(name);
        s += "\n";
    }
    return s;
}

std::string ToolsRegistry::join_tools() const {
    std::vector<std::string> names = list_tools();
    std::string s;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) s += ", ";
        s += names[i];
    }
    return s;
}

void ToolsRegistry::clear() {
    tools_.clear();
}

} // namespace coder
