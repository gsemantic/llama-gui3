// tools_registry.cpp — хранение ToolDef и генерация каталога из схем (И1.5).

#include "tools_registry.h"
#include "engine.h"
#include "json_utils.h"
#include "tool_protocol.h"
#include "limits.h"
#include "shell.h"

#include <filesystem>
#include <fstream>
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

/* --- И4.10: универсальное усечение вывода ---
 *
 * Правило одно и применяется к выводу ЛЮБОГО инструмента: не больше
 * limits::kMaxOutputLines строк и limits::kMaxOutputBytes байт, остальное
 * — в файл, путь возвращается в тексте ответа.
 *
 * Почему здесь, а не в ToolRunner::run: это единственная точка, через
 * которую проходит вывод каждого инструмента — и агента, и UI, и тестов.
 * Проверка в ToolRunner обходилась бы вторым вызывающим (панель
 * предпросмотра в UI вызывает реестр напрямую) и рано или поздно
 * обошлась бы, а забытый лимит стоит целого запроса к модели.
 *
 * Обрезается НАЧАЛО, а не конец: у результата инструмента (файл, grep,
 * список) полезнее первое, а у команд хвост и так остаётся в кольце
 * (core/shell.h, И4.8). Обрезанный вывод всегда объявляется словами:
 * иначе модель решит, что в файле было ровно столько, сколько вернули.
 */
void truncate_output(const std::string& tool_name, ToolOutput& out) {
    /* Инструмент, который умеет усекать сам, уже усек и объявил об этом
     * флагом. Повторная обрезка здесь отрезала бы то, что инструмент
     * показал НАМЕРЕННО: у bash это хвост вывода (И4.8), и обрезка по
     * началу уничтожила бы ровно то, ради чего кольцо и делалось. */
    if (out.truncated) return;

    if (out.output.size() <= limits::kMaxOutputBytes) {
        size_t lines = 0;
        for (char c : out.output) {
            if (c == '\n' && ++lines > limits::kMaxOutputLines) {
                out.truncated = true;
                break;
            }
        }
        if (!out.truncated) return;
    }

    /* Полный вывод — в файл. Пишем ДО обрезки, иначе «полный» окажется
     * обрезанным. */
    std::string spill;
    const auto& cb = Engine::instance().callbacks();
    if (cb.path_data_dir) spill = shell::next_spill_path(cb.path_data_dir(), tool_name);
    std::string kept = out.output;
    if (!spill.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(spill).parent_path(), ec);
        std::ofstream f(spill, std::ios::binary | std::ios::trunc);
        if (f.is_open()) {
            f << out.output;
            f.close();
        } else {
            spill.clear();   /* не смогли — не врём про путь */
        }
    }

    size_t bytes = 0, lines = 0, cut = std::string::npos;
    for (size_t i = 0; i < kept.size(); ++i) {
        if (kept[i] == '\n') {
            if (++lines >= limits::kMaxOutputLines) { cut = i + 1; break; }
        }
        if (++bytes >= limits::kMaxOutputBytes) { cut = i + 1; break; }
    }
    if (cut != std::string::npos) kept = kept.substr(0, cut);

    std::stringstream note;
    note << "\n[вывод обрезан: показано " << lines << " строк / " << bytes
         << " байт из " << out.output.size() << " байт]";
    if (!spill.empty())
        note << "\n[полный вывод: " << spill
             << " — прочитай его через read_file, если нужен целиком]";
    else
        note << "\n[полный вывод не сохранён: нет каталога данных]";
    out.output = kept + note.str();
    out.truncated = true;
    if (!out.metadata.is_object()) out.metadata = json::JsonValue::object();
    out.metadata.set("truncated", true);
    if (!spill.empty()) out.metadata.set("output_path", spill);
    out.metadata.set("total_bytes", static_cast<long long>(out.output.size()));
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
    truncate_output(tool_name, out);
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
    /* Динамическое описание ЗАМЕЩАЕТ статическое (И8.13): см. ToolDef. */
    const std::string description = def->describe_dynamic
                                        ? def->describe_dynamic()
                                        : def->description;
    if (!description.empty()) s += " — " + description;
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
