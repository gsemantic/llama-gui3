#include "test_framework.h"
#include "../core/engine.h"

using namespace coder;

TEST(engine_parse_action_json) {
    std::string block = "{\"tool\": \"read_file\", \"path\": \"wp-config.php\", \"k\": 10}";
    Engine::Action act;
    bool ok = Engine::parse_action(block, act);

    ASSERT_TRUE(ok);
    ASSERT_EQ(act.tool, std::string("read_file"));
    ASSERT_EQ(act.path, std::string("wp-config.php"));
    ASSERT_EQ(act.k, 10);
}

TEST(engine_parse_action_json_with_content) {
    std::string block = "{\"tool\": \"write_file\", \"path\": \".env\", "
                        "\"content\": \"DB_PASS=\\\"secret\\\\nvalue\\\"\"}";
    Engine::Action act;
    bool ok = Engine::parse_action(block, act);

    ASSERT_TRUE(ok);
    ASSERT_EQ(act.tool, std::string("write_file"));
    ASSERT_TRUE(act.content.find("DB_PASS=") != std::string::npos);
}

TEST(engine_extract_action_json_fenced) {
    std::string text = "Посмотрю файл.\n```json\n{\"tool\": \"read_file\", \"path\": \"a.txt\"}\n```\nДалее...";
    std::string rest;
    std::string block = Engine::extract_action(text, rest);

    ASSERT_TRUE(!block.empty());
    ASSERT_TRUE(block.find("\"tool\"") != std::string::npos);
    ASSERT_TRUE(rest.find("Посмотрю файл.") != std::string::npos);
    ASSERT_TRUE(rest.find("Далее...") != std::string::npos);
}

TEST(engine_parse_action_json_fallback_to_wp_action) {
    std::string block = "TOOL: grep_search\nPATTERN: \\d+\n";
    Engine::Action act;
    bool ok = Engine::parse_action(block, act);

    ASSERT_TRUE(ok);
    ASSERT_EQ(act.tool, std::string("grep_search"));
    ASSERT_EQ(act.pattern, std::string("\\d+"));
}

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

TEST(engine_system_prompt_stable_across_steps) {
    auto& eng = Engine::instance();
    /* C1: префикс системного промпта должен оставаться стабильным между шагами
     * (поставщик кэширует его). Повторный вызов без инвалидации — тот же текст. */
    eng.invalidate_prompt_cache();  // сбрасываем кэш
    std::string p1 = eng.build_system_prompt();
    std::string p2 = eng.build_system_prompt();
    ASSERT_TRUE(p1 == p2);
    /* План-режим пометить, что промпт стабилен даже при повторных вызовах. */
    eng.invalidate_prompt_cache();
    std::string p3 = eng.build_system_prompt();
    ASSERT_TRUE(p1 == p3);
}

TEST(engine_trim_session_compression) {
    auto& eng = Engine::instance();
    /* Наполняем сессию большими RESULT-сообщениями. */
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().session.clear();
        eng.state().session.push_back({"user", "задача"});
        for (int i = 0; i < 200; ++i) {
            eng.state().session.push_back({"assistant", "ответ " + std::to_string(i) +
                " " + std::string(500, 'x')});
            eng.state().session.push_back({"user", "RESULT [tool]:\n" +
                std::string(2000, 'y')});
        }
    }
    /* Вызываем сжатие (через тестовый обёртку). */
    eng.trim_session_test();

    /* Итоговый объём должен упасть ниже бюджета (60 Кб). */
    size_t total = 0;
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        for (const auto& m : eng.session_for_test()) total += m.content.size();
    }
    ASSERT_TRUE(total <= 60000);
    /* Старые RESULT должны быть заменены сжатой заглушкой с именем инструмента. */
    bool stub_found = false;
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        for (const auto& m : eng.session_for_test())
            if (m.content.find("сжат") != std::string::npos)
                stub_found = true;
    }
    ASSERT_TRUE(stub_found);
    /* В сжатой заглушке сохраняется имя инструмента (tool). */
    bool tool_kept = false;
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        for (const auto& m : eng.session_for_test())
            if (m.content.find("[RESULT tool сжат]") != std::string::npos)
                tool_kept = true;
    }
    ASSERT_TRUE(tool_kept);
}
