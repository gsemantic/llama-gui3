#include "test_framework.h"
#include "../core/module_api.h"

using namespace coder;

TEST(module_registry_register) {
    auto& reg = ModuleRegistry::instance();

    static CoderModule test_mod = {
        "test_module_alpha", "Test Alpha", "Test module alpha",
        nullptr,
        []() -> std::vector<ToolInfo> { return {}; },
        []() -> std::vector<Skill> {
            return {{"alpha_skill", "Alpha skill", "Body"}};
        },
        []() -> const char* { return "PROMPT: Alpha module"; },
        nullptr, nullptr, nullptr, nullptr
    };

    reg.register_module(&test_mod);

    const CoderModule* found = reg.find("test_module_alpha");
    ASSERT_TRUE(found != nullptr);
    ASSERT_EQ(std::string(found->display_name), std::string("Test Alpha"));
}

TEST(module_registry_find_nonexistent) {
    auto& reg = ModuleRegistry::instance();
    const CoderModule* found = reg.find("nonexistent_xyz");
    ASSERT_TRUE(found == nullptr);
}

TEST(module_registry_all_skills) {
    auto& reg = ModuleRegistry::instance();
    auto skills = reg.all_skills();
    /* Должен содержать навыки от test_module_alpha. */
    bool found = false;
    for (const auto& sk : skills) {
        if (sk.name == "alpha_skill") { found = true; break; }
    }
    ASSERT_TRUE(found);
}

TEST(module_registry_combined_prompt) {
    auto& reg = ModuleRegistry::instance();
    std::string prompt = reg.combined_system_prompt();
    ASSERT_TRUE(prompt.find("Alpha module") != std::string::npos);
}

TEST(module_registry_modules_list) {
    auto& reg = ModuleRegistry::instance();
    const auto& mods = reg.modules();
    ASSERT_TRUE(mods.size() >= 1);
}

TEST(tool_args_defaults) {
    ToolArgs args;
    ASSERT_EQ(args.k, 6);
    ASSERT_TRUE(args.path.empty());
    ASSERT_TRUE(args.query.empty());
}
