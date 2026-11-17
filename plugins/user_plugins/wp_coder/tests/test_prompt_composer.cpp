/*
 * test_prompt_composer.cpp — контракт порядка системного промпта (P1).
 *
 * Порядок слоёв — не косметика: он определяет, что модель прочитает
 * раньше, и какой хвост меняется на каждом шаге (кэш префикса). Поэтому
 * проверяется независимо от Engine: compose_prompt обязан расставить
 * блоки по enum, а не по порядку добавления.
 */

#include "test_framework.h"
#include "../core/prompt_composer.h"

using namespace coder;

TEST(prompt_composer_orders_layers_regardless_of_insertion_order) {
    std::vector<PromptBlock> blocks = {
        {PromptLayer::Skills, "SKILLS"},
        {PromptLayer::Mode, "MODE"},
        {PromptLayer::Base, "BASE"},
        {PromptLayer::Plan, "PLAN"},
        {PromptLayer::Instructions, "AGENTS"},
        {PromptLayer::ProjectRoot, "ROOT"},
        {PromptLayer::Tools, "TOOLS"},
        {PromptLayer::Environment, "ENV"},
        {PromptLayer::Module, "MODULE"},
    };
    const std::string out = compose_prompt(std::move(blocks));

    const size_t root = out.find("ROOT");
    const size_t base = out.find("BASE");
    const size_t env = out.find("ENV");
    const size_t agents = out.find("AGENTS");
    const size_t module = out.find("MODULE");
    const size_t tools = out.find("TOOLS");
    const size_t skills = out.find("SKILLS");
    const size_t plan = out.find("PLAN");
    const size_t mode = out.find("MODE");

    for (size_t p : {root, base, env, agents, module, tools, skills, plan, mode}) {
        ASSERT_TRUE(p != std::string::npos);
    }
    ASSERT_TRUE(root < base);
    ASSERT_TRUE(base < env);
    ASSERT_TRUE(env < agents);
    ASSERT_TRUE(agents < module);
    ASSERT_TRUE(module < tools);
    ASSERT_TRUE(tools < skills);
    ASSERT_TRUE(skills < plan);
    ASSERT_TRUE(plan < mode);
}

TEST(prompt_composer_keeps_insertion_order_inside_one_layer) {
    /* Несколько инструкций проекта — это разные файлы, и их порядок задан
     * discovery. Сортировка не должна его перемешивать. */
    std::vector<PromptBlock> blocks = {
        {PromptLayer::Instructions, "FIRST"},
        {PromptLayer::Base, "BASE"},
        {PromptLayer::Instructions, "SECOND"},
    };
    const std::string out = compose_prompt(std::move(blocks));
    ASSERT_TRUE(out.find("FIRST") < out.find("SECOND"));
}

TEST(prompt_composer_skips_empty_blocks_and_extra_newlines) {
    std::vector<PromptBlock> blocks = {
        {PromptLayer::Base, "BASE"},
        {PromptLayer::Skills, ""},
        {PromptLayer::Plan, "\n\n\n"},
        {PromptLayer::Mode, "\nMODE\n"},
    };
    const std::string out = compose_prompt(std::move(blocks));
    ASSERT_EQ(out, std::string("BASE\n\nMODE"));
}
