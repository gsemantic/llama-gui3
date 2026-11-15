/*
 * test_tools_registry.cpp — реестр инструментов на типизированных аргументах.
 *
 * После И1.3 реестр хранит ToolDef: имя, описание, JSON-схему параметров
 * и флаги. Проверяем здесь именно эту новую часть — регистрацию через
 * register_def, вызов с JsonValue-аргументами и то, что неизвестный
 * инструмент и инструмент без обработчика дают внятный отказ, а не
 * исключение.
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/json.h"
#include "../core/tools_registry.h"
#include "../core/tool.h"
#include "../core/engine.h"
#include "../core/base_tools.h"

using namespace coder;

static void register_echo() {
    ToolDef def;
    def.name = "test_echo";
    def.description = "Echo test tool";
    def.flags = TF_READ_ONLY;
    SchemaBuilder b;
    b.str("query", "что повторить").required("query");
    def.parameters = b.build();
    def.handler = [](const json::JsonValue& a, ToolContext&) -> ToolOutput {
        ToolOutput o;
        o.title = "echo";
        o.output = "echo:" + a.get_string("query");
        return o;
    };
    ToolsRegistry::instance().register_def(std::move(def));
}

TEST(tools_registry_register_and_run) {
    auto& reg = ToolsRegistry::instance();
    reg.clear();
    register_echo();

    ASSERT_TRUE(reg.has("test_echo"));
    ASSERT_FALSE(reg.has("nonexistent"));

    json::JsonValue args = json::JsonValue::object();
    args.set("query", "hello");
    ASSERT_EQ(reg.run("test_echo", args), std::string("echo:hello"));

    /* Старый путь из 8 слотов идёт через ту же валидацию и вызов. */
    ToolArgs legacy;
    legacy.query = "hello";
    ASSERT_EQ(reg.run("test_echo", legacy), std::string("echo:hello"));
}

/* Инструмент запоминает модуль, который его регистрировал. Владелец
 * ставится ModuleRegistry::init_all, а не каждым объявлением: иначе
 * новый модуль забудет тег, и его инструменты останутся видимыми в
 * проекте другого домена. */
TEST(tools_registry_stamps_the_registration_module) {
    auto& reg = ToolsRegistry::instance();
    reg.clear();

    reg.set_registration_module("python");
    ToolDef py;
    py.name = "python_probe";
    py.description = "python tool";
    reg.register_def(std::move(py));
    reg.set_registration_module("");

    ToolDef core;
    core.name = "core_probe";
    core.description = "core tool";
    reg.register_def(std::move(core));

    ASSERT_TRUE(reg.find("python_probe") != nullptr);
    ASSERT_EQ(reg.find("python_probe")->module, std::string("python"));
    ASSERT_TRUE(reg.find("core_probe")->module.empty());
    reg.clear();
}

/* Активный модуль скрывает чужие инструменты, но не запрещает их:
 * каталог для калькулятора на Python не должен содержать wp_cli и
 * docker_build. Проверяется именно ВИДИМОСТЬ (visible_tools — единственный
 * источник и для промпта, и для нативных спецификаций). */
TEST(engine_visible_tools_hide_other_modules) {
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&,
                     LlmReply&) { return false; };
    cb.llm_complete = [](const std::string&, const std::string&,
                         std::string&) { return false; };
    cb.llm_is_connected = []() { return false; };
    cb.chat_event = [](const std::string&) {};
    auto& eng = Engine::instance();
    eng.init(cb);
    test_support::approve_all_permissions();

    auto& reg = ToolsRegistry::instance();
    reg.clear();
    register_base_tools();

    reg.set_registration_module("python");
    ToolDef py;
    py.name = "python_probe_visible";
    py.description = "python";
    reg.register_def(std::move(py));

    reg.set_registration_module("wordpress");
    ToolDef wp;
    wp.name = "wp_probe_hidden";
    wp.description = "wordpress";
    reg.register_def(std::move(wp));
    reg.set_registration_module("");

    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().active_module = "python";
    }
    const std::vector<std::string> visible = eng.visible_tools();
    bool has_py = false, has_wp = false, has_core = false;
    for (const std::string& n : visible) {
        if (n == "python_probe_visible") has_py = true;
        if (n == "wp_probe_hidden") has_wp = true;
        if (n == "read_file") has_core = true;
    }
    ASSERT_TRUE(has_py);
    ASSERT_TRUE(has_core);
    ASSERT_FALSE(has_wp);

    /* «generic» возвращает прежнее поведение: видны все домены. */
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().active_module = "generic";
    }
    has_wp = false;
    for (const std::string& n : eng.visible_tools()) {
        if (n == "wp_probe_hidden") has_wp = true;
    }
    ASSERT_TRUE(has_wp);
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().active_module.clear();
    }
}

TEST(tools_registry_list) {
    auto& reg = ToolsRegistry::instance();
    reg.clear();

    ToolDef a, b;
    a.name = "tool_a";
    a.description = "a";
    a.handler = [](const json::JsonValue&, ToolContext&) { return ToolOutput{}; };
    b.name = "tool_b";
    b.description = "b";
    b.handler = [](const json::JsonValue&, ToolContext&) { return ToolOutput{}; };
    reg.register_def(std::move(a));
    reg.register_def(std::move(b));

    auto list = reg.list_tools();
    ASSERT_EQ(list.size(), (size_t)2);
    /* find отдаёт описание, а не только факт регистрации. */
    const ToolDef* found = reg.find("tool_a");
    ASSERT_TRUE(found != nullptr);
    ASSERT_EQ(found->description, std::string("a"));
    ASSERT_TRUE(reg.find("tool_c") == nullptr);
}

TEST(tools_registry_unknown_tool) {
    auto& reg = ToolsRegistry::instance();
    reg.clear();

    ToolArgs args;
    std::string result = reg.run("does_not_exist", args);
    ASSERT_TRUE(result.find("неизвестный инструмент") != std::string::npos);
}

TEST(tools_registry_tool_without_handler_reports_error) {
    auto& reg = ToolsRegistry::instance();
    reg.clear();

    ToolDef def;
    def.name = "no_handler";
    def.description = "забыли обработчик";
    reg.register_def(std::move(def));

    ToolArgs args;
    std::string result = reg.run("no_handler", args);
    ASSERT_TRUE(result.find("без обработчика") != std::string::npos);
}

TEST(tools_registry_clear) {
    auto& reg = ToolsRegistry::instance();
    register_echo();
    ASSERT_TRUE(reg.has("test_echo"));
    reg.clear();
    ASSERT_FALSE(reg.has("test_echo"));
}

/* --- Каталог для промпта строится из схем (И1.5) --- */

TEST(tools_registry_catalogue_comes_from_schema) {
    auto& reg = ToolsRegistry::instance();
    reg.clear();
    register_echo();

    std::string entry = reg.describe_tool("test_echo");
    /* Имя, описание, имя параметра, его тип и обязательность. */
    ASSERT_TRUE(entry.find("echo") != std::string::npos);
    ASSERT_TRUE(entry.find("Echo test tool") != std::string::npos);
    ASSERT_TRUE(entry.find("query") != std::string::npos);
    ASSERT_TRUE(entry.find("текст") != std::string::npos);
    ASSERT_TRUE(entry.find("обязательно") != std::string::npos);
    /* Несуществующий инструмент — пустое описание, а не «- :». */
    ASSERT_EQ(reg.describe_tool("nope"), std::string(""));
}
