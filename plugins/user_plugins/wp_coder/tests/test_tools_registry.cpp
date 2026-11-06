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
#include "../core/json.h"
#include "../core/tools_registry.h"
#include "../core/tool.h"
#include "../core/engine.h"

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
