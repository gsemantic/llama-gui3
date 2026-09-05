#include "test_framework.h"
#include "../core/tools_registry.h"
#include "../core/engine.h"

using namespace coder;

TEST(tools_registry_register_and_run) {
    auto& reg = ToolsRegistry::instance();
    reg.clear();

    reg.register_tool("echo", [](const ToolArgs& a) -> std::string {
        return "echo:" + a.query;
    }, "Echo test tool");

    ASSERT_TRUE(reg.has("echo"));
    ASSERT_FALSE(reg.has("nonexistent"));

    ToolArgs args;
    args.query = "hello";
    std::string result = reg.run("echo", args);
    ASSERT_EQ(result, "echo:hello");
}

TEST(tools_registry_list) {
    auto& reg = ToolsRegistry::instance();
    reg.clear();

    reg.register_tool("tool_a", [](const ToolArgs&) { return "a"; }, "");
    reg.register_tool("tool_b", [](const ToolArgs&) { return "b"; }, "");

    auto list = reg.list_tools();
    ASSERT_TRUE(list.size() >= 2);
}

TEST(tools_registry_unknown_tool) {
    auto& reg = ToolsRegistry::instance();
    reg.clear();

    ToolArgs args;
    std::string result = reg.run("does_not_exist", args);
    ASSERT_TRUE(result.find("неизвестный инструмент") != std::string::npos);
}

TEST(tools_registry_clear) {
    auto& reg = ToolsRegistry::instance();
    reg.register_tool("temp_tool", [](const ToolArgs&) { return "temp"; }, "");
    ASSERT_TRUE(reg.has("temp_tool"));

    reg.clear();
    ASSERT_FALSE(reg.has("temp_tool"));
}
