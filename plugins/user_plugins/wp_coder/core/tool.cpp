// tool.cpp — ToolContext, сборка схем, валидация аргументов (И1.3–1.6).

#include "tool.h"
#include "engine.h"
#include "agent_components.h"
#include "command_policy.h"
#include "tools_registry.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

namespace coder {

/* И6.7: токен хода. nullptr, если ход без токена (тесты, служебные
 * вызовы) — инструмент обязан это трактовать как «отменять нечем». */
AbortToken* ToolContext::abort() const
{
    return state_ ? state_->turn_abort.get() : nullptr;
}

/* ======================================================================
 * Флаги
 * ====================================================================== */

const char* tool_flag_names(unsigned flags) {
    static thread_local std::string names;
    names.clear();
    struct Item { unsigned bit; const char* name; };
    static const Item items[] = {
        {TF_READ_ONLY,    "read_only"},
        {TF_WRITES_FILES, "writes_files"},
        {TF_EXECUTES,     "executes"},
        {TF_NETWORK,      "network"},
        {TF_DESTRUCTIVE,  "destructive"},
        {TF_SLOW,         "slow"},
        {TF_UNCLASSIFIED, "unclassified"},
    };
    for (const auto& it : items) {
        if (tf_has(flags, static_cast<ToolFlag>(it.bit))) {
            if (!names.empty()) names += "|";
            names += it.name;
        }
    }
    if (names.empty()) names = "none";
    return names.c_str();
}

/* ======================================================================
 * ToolContext
 * ====================================================================== */

EngineState& ToolContext::state() const { return *state_; }
HostCallbacks& ToolContext::callbacks() const { return *cb_; }

int ToolContext::mode() const {
    /* mode меняется из UI-потока, читаем под тем же мьютексом, что и его
     * записывают (state_.mtx — он же защищает остальное состояние). */
    std::lock_guard<std::mutex> lk(state_->mtx);
    return state_->mode;
}

bool ToolContext::plan_mode() const {
    std::lock_guard<std::mutex> lk(state_->mtx);
    return state_->plan_mode;
}

bool ToolContext::research_mode() const { return mode() == 1; }
bool ToolContext::review_mode() const { return mode() == 2; }

std::string ToolContext::project_dir() const {
    std::lock_guard<std::mutex> lk(state_->mtx);
    return state_->project_dir;
}

std::string ToolContext::php_bin() const {
    std::lock_guard<std::mutex> lk(state_->mtx);
    return state_->php_bin;
}

std::string ToolContext::check_external_permission(const std::string& abs_path) {
    /* PermissionGate внутри переводит агента в WaitingPermission и
     * возвращает текст отказа. Лок здесь НЕ держим — gate берёт
     * state_.mtx сам, а std::mutex нерекурсивный (дедлок D1).
     *
     * Событие «Требуется разрешение» публикуется через Engine: у него
     * единственная очередь событий и подписка UI. Дублировать очередь
     * в ToolContext означало бы второе место, куда UI не смотрит. */
    auto push = [](AgentEvent::Kind k, const std::string& t) {
        Engine::instance().push_event(k, t);
    };
    PermissionGate gate(*state_, *cb_, push);
    return gate.check(abs_path);
}

bool ToolContext::propose_write(const std::string& rel_path,
                                const std::string& content) {
    if (!plan_mode()) return false;
    std::lock_guard<std::mutex> lk(state_->mtx);
    state_->pending.push_back({rel_path, content});
    return true;
}

/* ======================================================================
 * Сборка схемы
 * ====================================================================== */

namespace {

json::JsonValue prop(const char* type, const char* description) {
    json::JsonValue p = json::JsonValue::object();
    p.set("type", type);
    if (description && description[0]) p.set("description", description);
    return p;
}

} // anonymous namespace

SchemaBuilder& SchemaBuilder::str(const char* name, const char* description) {
    props_.set(name, prop("string", description));
    return *this;
}

SchemaBuilder& SchemaBuilder::integer(const char* name, const char* description) {
    props_.set(name, prop("integer", description));
    return *this;
}

SchemaBuilder& SchemaBuilder::integer_range(const char* name,
                                            const char* description,
                                            long long min_value,
                                            long long max_value) {
    json::JsonValue p = prop("integer", description);
    p.set("minimum", min_value);
    p.set("maximum", max_value);
    props_.set(name, p);
    return *this;
}

SchemaBuilder& SchemaBuilder::number(const char* name, const char* description) {
    props_.set(name, prop("number", description));
    return *this;
}

SchemaBuilder& SchemaBuilder::boolean(const char* name, const char* description) {
    props_.set(name, prop("boolean", description));
    return *this;
}

SchemaBuilder& SchemaBuilder::object_array(const char* name,
                                           const char* description,
                                           const char* item_description) {
    json::JsonValue item = json::JsonValue::object();
    item.set("type", "object");
    if (item_description && item_description[0])
        item.set("description", item_description);
    json::JsonValue p = json::JsonValue::object();
    p.set("type", "array");
    if (description && description[0]) p.set("description", description);
    p.set("items", std::move(item));
    props_.set(name, p);
    return *this;
}

SchemaBuilder& SchemaBuilder::string_enum(const char* name, const char* description,
                                          const std::vector<std::string>& values) {
    json::JsonValue p = prop("string", description);
    json::JsonValue list = json::JsonValue::array();
    for (const auto& v : values) list.push_back(v);
    p.set("enum", list);
    props_.set(name, p);
    return *this;
}

SchemaBuilder& SchemaBuilder::required(const char* name) {
    required_.push_back(name);
    return *this;
}

SchemaBuilder& SchemaBuilder::strict() {
    strict_ = true;
    return *this;
}

json::JsonValue SchemaBuilder::build() const {
    json::JsonValue schema = json::JsonValue::object();
    schema.set("type", "object");
    schema.set("properties", props_);
    schema.set("required", required_);
    schema.set("additionalProperties", !strict_);
    return schema;
}

json::JsonValue schema_path_only(const char* description) {
    SchemaBuilder b;
    b.str("path", description).required("path");
    return b.build();
}

/* ======================================================================
 * Валидация аргументов (И1.6)
 * ====================================================================== */

namespace {

/* Человекочитаемое имя типа в сообщении модели. */
std::string describe(const json::JsonValue& v) {
    switch (v.type()) {
        case json::JsonValue::Type::Null:   return "null";
        case json::JsonValue::Type::Bool:   return "true/false";
        case json::JsonValue::Type::Int:    return "integer";
        case json::JsonValue::Type::Double: return "number";
        case json::JsonValue::Type::String: return "string";
        case json::JsonValue::Type::Array:  return "array";
        case json::JsonValue::Type::Object: return "object";
    }
    return "value";
}

/* Перечислить ключи объекта для сообщения: "path, root, pattern". */
std::string join_keys(const std::vector<std::string>& keys) {
    std::string s;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i) s += ", ";
        s += keys[i];
    }
    return s;
}

/* Перечислить значения enum: "a, b, c". */
std::string join_enum(const json::JsonValue& list) {
    std::string s;
    for (size_t i = 0; i < list.size(); ++i) {
        if (i) s += ", ";
        s += list.at(i).as_string();
    }
    return s;
}

/* Число в сообщении модели: целые без дробной части. */
std::string number_text(const json::JsonValue& v) {
    if (v.type() == json::JsonValue::Type::Int) return std::to_string(v.as_int());
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%g", v.as_double());
    return buf;
}

/* Соответствие типа схеме.
 *
 * Числа и логические значения допускаются в «приводимом» виде: локальные
 * модели стабильно присылают "5" вместо 5 и "true" вместо true, а
 * обработчик всё равно прочитает их через get_int/as_bool. Отклонять
 * такой вызов значит заставлять модель тратить шаг на исправление
 * того, что и так сработает. Строки, наоборот, не приводятся:
 * number вместо path — это ошибка, а не формат. */
bool type_matches(const std::string& expected, const json::JsonValue& v) {
    using T = json::JsonValue::Type;
    if (expected == "string") return v.type() == T::String;
    if (expected == "integer") {
        if (v.type() == T::Int) return true;
        if (v.type() != T::String) return false;
        /* Строка должна целиком быть числом, иначе это мусор. */
        const std::string& s = v.as_string();
        if (s.empty()) return false;
        char* end = nullptr;
        std::strtoll(s.c_str(), &end, 10);
        return end != s.c_str() && *end == '\0';
    }
    if (expected == "number") {
        if (v.is_number()) return true;
        if (v.type() != T::String) return false;
        const std::string& s = v.as_string();
        if (s.empty()) return false;
        char* end = nullptr;
        std::strtod(s.c_str(), &end);
        return end != s.c_str() && *end == '\0';
    }
    if (expected == "boolean") {
        return v.type() == T::Bool || v.type() == T::String
            || v.type() == T::Int;
    }
    if (expected == "array") return v.type() == T::Array;
    if (expected == "object") return v.type() == T::Object;
    /* Неизвестный тип в схеме — не повод отклонять вызов. */
    return true;
}

/* Проверка одного значения по описанию свойства. */
bool check_property(const std::string& name, const json::JsonValue& schema,
                    const json::JsonValue& value, std::string& detail) {
    const std::string expected = schema.get_string("type");
    if (!expected.empty() && !type_matches(expected, value)) {
        detail = "parameter '" + name + "' must be " + expected
               + ", got " + describe(value);
        return false;
    }
    if (schema.has("enum")) {
        const json::JsonValue& list = schema.get("enum");
        bool found = false;
        for (size_t i = 0; i < list.size(); ++i) {
            if (list.at(i).as_string() == value.as_string()) { found = true; break; }
        }
        if (!found) {
            detail = "parameter '" + name + "' must be one of ["
                   + join_enum(list) + "], got '" + value.as_string() + "'";
            return false;
        }
    }
    if (value.is_number() && schema.has("minimum")) {
        if (value.as_double() < schema.get_double("minimum")) {
            detail = "parameter '" + name + "' must be >= "
                   + number_text(schema.get("minimum"))
                   + ", got " + number_text(value);
            return false;
        }
    }
    if (value.is_number() && schema.has("maximum")) {
        if (value.as_double() > schema.get_double("maximum")) {
            detail = "parameter '" + name + "' must be <= "
                   + number_text(schema.get("maximum"))
                   + ", got " + number_text(value);
            return false;
        }
    }
    return true;
}

} // anonymous namespace

bool validate_tool_args(const json::JsonValue& parameters,
                        const json::JsonValue& args,
                        std::string& detail) {
    detail.clear();
    /* Схемы нет — валидировать нечего (старый инструмент без схемы). */
    if (parameters.is_null() || !parameters.is_object()) return true;
    if (!args.is_object()) {
        detail = "arguments must be a JSON object";
        return false;
    }

    const json::JsonValue& props = parameters.get("properties");
    if (parameters.has("required")) {
        const json::JsonValue& req = parameters.get("required");
        for (size_t i = 0; i < req.size(); ++i) {
            const std::string name = req.at(i).as_string();
            if (name.empty() || args.has(name)) continue;
            detail = "missing required parameter '" + name + "'";
            if (props.is_object() && props.size() > 0) {
                detail += "; expected parameters: " + join_keys(props.keys());
            }
            return false;
        }
    }

    if (props.is_object()) {
        for (const auto& entry : props.entries()) {
            if (!args.has(entry.first)) continue;
            if (!check_property(entry.first, *entry.second,
                                args.get(entry.first), detail)) {
                return false;
            }
        }
    }

    /* Неизвестные ключи: по умолчанию разрешены (см. SchemaBuilder::strict),
     * строгий режим включается осознанно. */
    if (parameters.get_bool("additionalProperties", true) == false) {
        for (const auto& key : args.keys()) {
            if (props.is_object() && props.has(key)) continue;
            if (key == "tool") continue;   /* имя инструмента — не параметр */
            detail = "unknown parameter '" + key + "'";
            if (props.is_object() && props.size() > 0) {
                detail += "; expected parameters: " + join_keys(props.keys());
            }
            return false;
        }
    }
    return true;
}

std::string invalid_arguments_message(const std::string& tool_name,
                                      const std::string& detail) {
    return "The " + tool_name
         + " tool was called with invalid arguments: " + detail
         + ". Please rewrite the input so it satisfies the expected schema.";
}

/* ======================================================================
 * Политика режимов (И1.7)
 * ====================================================================== */

namespace {

/* Текст отказа. Обязан быть actionable: модель должна и сообщить
 * пользователю причину, и не начать подбирать другой инструмент наугад,
 * поэтому здесь перечисляются конкретные нарушенные флаги. */
std::string make_refusal(const std::string& tool_name, unsigned flags,
                         const char* mode_name, const char* tail) {
    std::string reasons;
    auto add = [&reasons](const char* what) {
        if (!reasons.empty()) reasons += ", ";
        reasons += what;
    };
    if (tf_has(flags, TF_WRITES_FILES)) add("меняет файлы");
    if (tf_has(flags, TF_EXECUTES)) add("запускает код");
    if (tf_has(flags, TF_DESTRUCTIVE)) add("делает необратимые изменения");
    if (reasons.empty()) add("не помечен как read_only");
    return "[запрещено режимом] Инструмент " + tool_name + ": " + reasons
         + " (" + tail + "), а режим " + mode_name + " этого не допускает."
         + "\nНЕ ПОВТОРЯЙ вызов и не подменяй его другим инструментом."
           " Скажи пользователю, что задача требует режима Code"
           " (Настройки → Режим), и дождись его решения."
         + "\nПродолжай работу теми инструментами, которые доступны.";
}

} // anonymous namespace

std::string check_tool_mode_policy(const std::string& tool_name, unsigned flags,
                                   int mode, bool plan_mode) {
    if (tf_has(flags, TF_UNCLASSIFIED)) {
        return "[запрещено режимом] Инструмент " + tool_name
             + " не классифицирован (флаги не заданы), поэтому в режимах"
               " Research и «сначала план» он недоступен."
             + "\nНЕ ПОВТОРЯЙ вызов. Работай доступными инструментами.";
    }
    if (mode != 1 && !plan_mode) return "";

    /* Research: пропускаем только явно помеченные «ничего не меняют».
     * Считать EXECUTES запретом нельзя — git_status тоже запускает
     * команду, но он читает, и запрещать его в режиме «только чтение»
     * бессмысленно. */
    const bool research = (mode == 1);
    if (research) {
        if (!tf_has(flags, TF_READ_ONLY)) {
            return make_refusal(tool_name, flags, "Research (только чтение)",
                                "меняет состояние: файлы, код или данные");
        }
        return "";
    }
    if ((flags & kPlanForbidden) == 0u) return "";

    return make_refusal(tool_name, flags, "«сначала план»",
                        "запускает код или делает необратимые изменения");
}

std::string check_tool_mode_policy(const std::string& tool_name,
                                   const ToolDef& def, int mode, bool plan_mode) {
    return check_tool_mode_policy(tool_name, def.flags, mode, plan_mode);
}

/* --- И2.5/2.9: значение и ключ разрешения --- */

std::string permission_key_of(const ToolDef& def) {
    return def.permission_key.empty() ? def.name : def.permission_key;
}

std::vector<std::string> visible_tool_names(
        const std::vector<ToolDef>& all,
        const std::function<bool(const std::string&)>& denied_whole_key) {
    std::vector<std::string> out;
    out.reserve(all.size());
    for (const ToolDef& def : all) {
        if (denied_whole_key(permission_key_of(def))) continue;
        out.push_back(def.name);
    }
    return out;
}

std::string canonical_permission_key(const std::string& name, bool* known) {
    if (known) *known = false;
    if (name.empty()) return name;

    const std::vector<ToolDef> all = ToolsRegistry::instance().defs();
    for (const ToolDef& d : all) {
        if (d.name != name) continue;
        if (known) *known = true;
        return permission_key_of(d);
    }
    /* Имя уже является ключом («bash», «read», «write»): правило,
     * написанное для группы, должно работать и когда инструментов в
     * группе несколько (все пять пишущих инструментов — ключ
     * «write», и «bash: запретить» не должен означать «один
     * конкретный инструмент запрещён»). */
    for (const ToolDef& d : all) {
        if (permission_key_of(d) != name) continue;
        if (known) *known = true;
        return name;
    }
    /* Псевдонимы словаря порта: в opencode редактирование — это
     * `edit`, и оно покрывает write/edit/patch одним ключом. У нас
     * ключ этой группы называется `write` (см. permission_key у
     * write_file/apply_patch/search_replace/edit_file/undo_edit), и
     * менять его означало бы обнулить уже сохранённые правила
     * пользователя в настройках. Поэтому приводим ЧУЖИЕ имена к
     * нашему ключу, а не свой — к чужому. */
    static const std::vector<std::pair<std::string, std::string>> kAliases = {
        {"edit", "write"},
        {"multiedit", "write"},
        {"patch", "write"},
        {"apply_patch", "write"},
    };
    for (const auto& kv : kAliases) {
        if (kv.first != name) continue;
        if (known) *known = true;
        return kv.second;
    }
    return name;
}

std::string permission_pattern(const ToolDef& def, const json::JsonValue& args) {
    /* Порядок = приоритет. «path» раньше «url»: у одного инструмента
     * могут быть оба (скажем, скачать файл по URL в путь), и вопрос
     * должен звучать про то, что инструмент меняет, а не откуда он
     * это взял. */
    static const char* kPatternArgs[] = {
        "path", "file", "filePath", "command", "cmd", "cli",
        "url", "host", "query", "name", "branch", "site"
    };
    if (!args.is_object()) return "*";
    for (const char* key : kPatternArgs) {
        if (!args.has(key)) continue;
        const json::JsonValue& v = args.get(key);
        if (!v.is_string()) continue;
        const std::string s = v.as_string();
        if (!s.empty()) return s;
    }
    /* Нечего предъявлять — решение по факту вызова инструмента. */
    return "*";
}

std::string permission_suggested_pattern(const ToolDef& def,
                                         const std::string& pattern) {
    if (pattern.empty() || pattern == "*") return "*";
    /* И3.5: когда значение — это команда, «всегда» = безопасный префикс.
     * «Всегда разрешить git status --porcelain» не должно разрешить
     * git push --force, поэтому CommandPolicy::always_pattern сужает
     * шаблон до «бинарник + подкоманда». Для путей и URL значение
     * остаётся как есть: сужать каталог до подкаталога нельзя, это
     * был бы другой инструмент. */
    static const char* kCommandKeys[] = {"bash", "docker", "cron", "ssh", "deploy"};
    const std::string key = permission_key_of(def);
    for (const char* k : kCommandKeys) {
        if (key == k) return command_policy().always_pattern(pattern);
    }
    return pattern;
}

} // namespace coder
