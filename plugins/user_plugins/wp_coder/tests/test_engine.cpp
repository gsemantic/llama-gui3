#include "test_framework.h"
#include "../core/engine.h"

using namespace coder;

TEST(engine_extract_action_basic) {
    std::string text = "Some text before\n```\nwp_action\nTOOL: read_file\nPATH: test.php\n```\nText after";
    std::string rest;
    std::string block = Engine::extract_action(text, rest);

    ASSERT_TRUE(!block.empty());
    ASSERT_TRUE(block.find("TOOL: read_file") != std::string::npos);
    ASSERT_TRUE(block.find("PATH: test.php") != std::string::npos);
    ASSERT_TRUE(rest.find("Some text before") != std::string::npos);
    ASSERT_TRUE(rest.find("Text after") != std::string::npos);
}

TEST(engine_extract_action_with_fenced) {
    std::string text = "Before\n```wp_action\nTOOL: grep_search\nROOT: /src\nPATTERN: TODO\n```\nAfter";
    std::string rest;
    std::string block = Engine::extract_action(text, rest);

    ASSERT_TRUE(!block.empty());
    ASSERT_TRUE(block.find("TOOL: grep_search") != std::string::npos);
}

TEST(engine_extract_action_no_block) {
    std::string text = "Just plain text without any action blocks.";
    std::string rest;
    std::string block = Engine::extract_action(text, rest);

    ASSERT_TRUE(block.empty());
}

TEST(engine_parse_action_simple) {
    std::string block = "TOOL: read_file\nPATH: wp-config.php\n";
    Engine::Action act;
    bool ok = Engine::parse_action(block, act);

    ASSERT_TRUE(ok);
    ASSERT_EQ(act.tool, std::string("read_file"));
    ASSERT_EQ(act.path, std::string("wp-config.php"));
}

TEST(engine_parse_action_with_content) {
    std::string block = "TOOL: write_file\nPATH: test.txt\nCONTENT_BEGIN\nHello World\nSecond line\nCONTENT_END\n";
    Engine::Action act;
    bool ok = Engine::parse_action(block, act);

    ASSERT_TRUE(ok);
    ASSERT_EQ(act.tool, std::string("write_file"));
    ASSERT_EQ(act.path, std::string("test.txt"));
    ASSERT_TRUE(act.content.find("Hello World") != std::string::npos);
    ASSERT_TRUE(act.content.find("Second line") != std::string::npos);
}

TEST(engine_parse_action_all_params) {
    std::string block = "TOOL: some_tool\nPATH: /a/b\nROOT: /c\nQUERY: search term\nPATTERN: \\d+\nCLI: --flag\nURL: http://example.com\nK: 10\n";
    Engine::Action act;
    bool ok = Engine::parse_action(block, act);

    ASSERT_TRUE(ok);
    ASSERT_EQ(act.tool, std::string("some_tool"));
    ASSERT_EQ(act.path, std::string("/a/b"));
    ASSERT_EQ(act.root, std::string("/c"));
    ASSERT_EQ(act.query, std::string("search term"));
    ASSERT_EQ(act.pattern, std::string("\\d+"));
    ASSERT_EQ(act.cli, std::string("--flag"));
    ASSERT_EQ(act.url, std::string("http://example.com"));
    ASSERT_EQ(act.k, 10);
}

TEST(engine_parse_action_empty_tool) {
    std::string block = "PATH: some/path\n";
    Engine::Action act;
    bool ok = Engine::parse_action(block, act);

    ASSERT_FALSE(ok);
}

TEST(engine_build_system_prompt) {
    auto& eng = Engine::instance();
    std::string prompt = eng.build_system_prompt();
    ASSERT_TRUE(!prompt.empty());
    ASSERT_TRUE(prompt.find("инструмент") != std::string::npos);
}
