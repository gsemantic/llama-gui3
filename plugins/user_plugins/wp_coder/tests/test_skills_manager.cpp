#include "test_framework.h"
#include "../core/skills_manager.h"
#include "../core/module_api.h"
#include "../core/engine.h"

#include <fstream>
#include <filesystem>

using namespace coder;
namespace fs = std::filesystem;

TEST(skills_manager_load_from_modules) {
    auto& mgr = SkillsManager::instance();

    /* Регистрируем тестовый модуль с навыком. */
    static std::vector<Skill> test_skills = {
        {"test_skill_a", "Test skill A", "Body A"},
        {"test_skill_b", "Test skill B", "Body B"}
    };

    static CoderModule test_mod = {
        "test_mod", "Test", "Test module",
        nullptr,
        []() -> std::vector<ToolInfo> { return {}; },
        []() -> std::vector<Skill> { return test_skills; },
        nullptr, nullptr, nullptr, nullptr, nullptr
    };

    ModuleRegistry::instance().register_module(&test_mod);
    mgr.load();

    const auto& skills = mgr.all_skills();
    ASSERT_TRUE(skills.size() >= 2);
}

TEST(skills_manager_toggle) {
    auto& mgr = SkillsManager::instance();

    mgr.toggle("test_skill_a", true);
    const auto& active = mgr.active_skills();
    bool found = false;
    for (const auto& n : active) {
        if (n == "test_skill_a") { found = true; break; }
    }
    ASSERT_TRUE(found);

    mgr.toggle("test_skill_a", false);
    found = false;
    for (const auto& n : active) {
        if (n == "test_skill_a") { found = true; break; }
    }
    ASSERT_FALSE(found);
}

TEST(skills_manager_build_prompt) {
    auto& mgr = SkillsManager::instance();
    mgr.toggle("test_skill_b", true);

    std::string prompt = mgr.build_skills_prompt();
    ASSERT_TRUE(prompt.find("test_skill_b") != std::string::npos);
    ASSERT_TRUE(prompt.find("Body B") != std::string::npos);

    mgr.toggle("test_skill_b", false);
}

TEST(skills_manager_load_from_directory) {
    /* Создаём временный каталог с .md файлом. */
    std::string tmp_dir = "/tmp/wp_coder_test_skills";
    fs::create_directories(tmp_dir);

    {
        std::ofstream f(tmp_dir + "/my_test_skill.md");
        f << "# My Test Skill\nDescription of skill\nBody content here\n";
    }

    auto& mgr = SkillsManager::instance();
    size_t before = mgr.all_skills().size();
    mgr.load_from_directory(tmp_dir);
    size_t after = mgr.all_skills().size();

    ASSERT_TRUE(after > before);

    /* Cleanup. */
    fs::remove_all(tmp_dir);
}

TEST(skills_manager_find) {
    auto& mgr = SkillsManager::instance();
    const Skill* sk = mgr.find("test_skill_a");
    ASSERT_TRUE(sk != nullptr);
    ASSERT_EQ(sk->name, std::string("test_skill_a"));

    const Skill* missing = mgr.find("nonexistent_skill_xyz");
    ASSERT_TRUE(missing == nullptr);
}
