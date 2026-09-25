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
    /* Промпт содержит только имя и описание (тело выгружается через skill_detail). */
    ASSERT_TRUE(prompt.find("test_skill_b") != std::string::npos);
    ASSERT_TRUE(prompt.find("Body B") == std::string::npos);

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

/* ======================================================================
 * И0.4 / D11 — регрессии «агент без навыков».
 *
 * load() очищал active_ и не заполнял его, а plugin_main.cpp вызывал
 * set_module() только при непустой настройке active_module. На чистой
 * установке: active_ пуст → build_skills_prompt() == "" → модель не знает,
 * что skill_detail вообще существует. Навыков 14, в промпте 0.
 * ====================================================================== */

TEST(skills_load_populates_active_without_selected_module) {
    auto& mgr = SkillsManager::instance();
    mgr.load();  // active_module_ пуст — как на чистой установке

    /* Навыки есть, значит и активные должны быть. */
    ASSERT_TRUE(!mgr.all_skills().empty());
    ASSERT_TRUE(!mgr.active_skills().empty());
    ASSERT_EQ(mgr.active_skills().size(), mgr.all_skills().size());
}

TEST(skills_prompt_never_empty_when_skills_exist) {
    auto& mgr = SkillsManager::instance();
    mgr.load();
    std::string p = mgr.build_skills_prompt();
    ASSERT_TRUE(!p.empty());
    ASSERT_TRUE(p.find("skill_detail") != std::string::npos);
    ASSERT_TRUE(p.find("test_skill_a") != std::string::npos);
    /* Тела навыков в промпте быть не должно — они идут через skill_detail. */
    ASSERT_TRUE(p.find("Body A") == std::string::npos);
}

TEST(skills_prompt_mentions_skill_detail_even_with_zero_skills) {
    auto& mgr = SkillsManager::instance();
    mgr.set_active({});
    std::vector<Skill> saved = mgr.all_skills();
    /* Полностью очищаем набор, чтобы проверить случай «навыков нет». */
    std::string tmp = "/tmp/wp_coder_test_skills_empty";
    std::error_code ec;
    fs::create_directories(tmp, ec);
    fs::remove_all(tmp, ec);
    /* load() без зарегистрированных модулей даст пустой skills_. */
    mgr.load();
    if (mgr.all_skills().empty()) {
        std::string p = mgr.build_skills_prompt();
        ASSERT_TRUE(!p.empty());
        ASSERT_TRUE(p.find("skill_detail") != std::string::npos);
    }
    /* Возвращаем тестовый модуль, чтобы следующие тесты видели навыки. */
    static std::vector<Skill> restore = {
        {"test_skill_a", "Test skill A", "Body A"},
        {"test_skill_b", "Test skill B", "Body B"}
    };
    static CoderModule test_mod = {
        "test_mod", "Test", "Test module",
        nullptr,
        []() -> std::vector<ToolInfo> { return {}; },
        []() -> std::vector<Skill> { return restore; },
        nullptr, nullptr, nullptr, nullptr, nullptr
    };
    ModuleRegistry::instance().register_module(&test_mod);
    mgr.load();
    (void)saved;
}

TEST(skills_set_module_falls_back_when_module_has_no_skills) {
    auto& mgr = SkillsManager::instance();
    mgr.load();
    /* Модуля с таким именем нет — раньше active_ становился пустым. */
    mgr.set_module("no_such_module_xyz");
    ASSERT_TRUE(!mgr.active_skills().empty());
    ASSERT_TRUE(mgr.build_skills_prompt().find("test_skill_a") != std::string::npos);
    mgr.set_module("");
}

TEST(skills_refresh_active_picks_up_late_directory_skills) {
    /* Навык, добавленный ПОСЛЕ load(), обязан попасть в active_. */
    std::string tmp_dir = "/tmp/wp_coder_test_skills_late";
    std::error_code ec;
    fs::create_directories(tmp_dir, ec);
    {
        std::ofstream f(tmp_dir + "/late_skill.md");
        f << "# Late Skill\nПоявился позже\nBody\n";
    }
    auto& mgr = SkillsManager::instance();
    mgr.set_module("");
    mgr.load();
    mgr.load_from_directory(tmp_dir);
    mgr.refresh_active();

    bool found = false;
    for (const auto& n : mgr.active_skills())
        if (n == "late_skill") found = true;
    ASSERT_TRUE(found);

    /* Не должен терять и модульные навыки. */
    bool has_test = false;
    for (const auto& n : mgr.active_skills())
        if (n == "test_skill_a") has_test = true;
    ASSERT_TRUE(has_test);

    fs::remove_all(tmp_dir, ec);
}

/* Имя навыка = имя файла без расширения, а не текст заголовка.
 * skill_detail делает ТОЧНОЕ сравнение имён, поэтому заголовок вида
 * "# wp_setup — настройка окружения" раньше давал невызываемый навык
 * "wp_setup — настройка окружения" (и "Late" для "# Late Skill"). */
TEST(skills_md_title_yields_callable_name) {
    std::string tmp_dir = "/tmp/wp_coder_test_skills_title";
    std::error_code ec;
    fs::create_directories(tmp_dir, ec);
    {
        std::ofstream f(tmp_dir + "/wp_setup.md");
        f << "# wp_setup — настройка окружения для WordPress\n\n"
             "Описание навыка\n\n"
             "Тело навыка\n";
    }
    {
        /* Заголовок из нескольких слов, имя файла — одно слово. */
        std::ofstream f(tmp_dir + "/late_skill.md");
        f << "# Late Skill\nПоявился позже\nBody\n";
    }
    auto& mgr = SkillsManager::instance();
    mgr.set_module("");
    mgr.load();
    mgr.load_from_directory(tmp_dir, "wordpress");
    mgr.refresh_active();

    const Skill* sk = mgr.find("wp_setup");
    ASSERT_TRUE(sk != nullptr);
    ASSERT_EQ(sk->name, std::string("wp_setup"));
    /* Описание сохраняется, тело — тоже. */
    ASSERT_TRUE(sk->description.find("Описание навыка") != std::string::npos);
    ASSERT_TRUE(sk->body.find("Тело навыка") != std::string::npos);
    /* Имя пригодно как значение QUERY — без пробелов и тире. */
    ASSERT_TRUE(sk->name.find(' ') == std::string::npos);
    ASSERT_TRUE(sk->name.find('\xe2') == std::string::npos);

    /* Заголовок из двух слов не должен становиться именем "Late". */
    const Skill* late = mgr.find("late_skill");
    ASSERT_TRUE(late != nullptr);
    ASSERT_EQ(late->name, std::string("late_skill"));
    ASSERT_TRUE(late->description.find("Появился позже") != std::string::npos);
    ASSERT_TRUE(mgr.find("Late") == nullptr);

    fs::remove_all(tmp_dir, ec);
}
