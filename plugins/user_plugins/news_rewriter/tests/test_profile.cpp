#include "test_framework.h"

#include <algorithm>
#include <filesystem>
#include <string>

#include "config.h"
#include "profile.h"

using namespace news_rewriter;

namespace {

// Временный каталог для профилей (удаляется в деструкторе).
struct TmpDir {
    std::filesystem::path path;
    TmpDir() {
        path = std::filesystem::temp_directory_path() /
               ("nr_profiles_test_" + std::to_string(std::time(nullptr)) +
                "_" + std::to_string(std::hash<const char*>()(__FILE__) % 100000));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TmpDir() { std::filesystem::remove_all(path); }
    std::string str() const { return path.string(); }
};

static void test_profile_save_and_load_roundtrip() {
    TmpDir dir;
    Config cfg = default_config();
    cfg.schedule_minutes = 15;
    cfg.rewrite.language = "en";
    cfg.sources.push_back(
        SourceConfig{"https://x.example/rss", "rss", SourceExtract{}, true});

    TEST_ASSERT_TRUE(save_profile(dir.str(), "Рабочий профиль!", cfg));

    // Имя файла — безопасный slug, внутри хранится оригинальное имя.
    TEST_ASSERT_EQUAL(active_profile_name(dir.str()), "Рабочий профиль!");
    std::vector<std::string> list = list_profiles(dir.str());
    TEST_ASSERT_EQUAL(list.size(), std::size_t(1));
    TEST_ASSERT_EQUAL(list[0], "Рабочий профиль!");

    const Config back = load_profile(dir.str(), "Рабочий профиль!");
    TEST_ASSERT_EQUAL(back.schedule_minutes, 15);
    TEST_ASSERT_EQUAL(back.rewrite.language, "en");
    TEST_ASSERT_EQUAL(back.sources.size(), std::size_t(2));
}

static void test_profile_multiple_and_delete() {
    TmpDir dir;
    Config a = default_config();
    a.schedule_minutes = 1;
    Config b = default_config();
    b.schedule_minutes = 2;
    TEST_ASSERT_TRUE(save_profile(dir.str(), "A", a));
    TEST_ASSERT_TRUE(save_profile(dir.str(), "B", b));

    auto list = list_profiles(dir.str());
    TEST_ASSERT_EQUAL(list.size(), std::size_t(2));
    // Отсортировано по имени.
    TEST_ASSERT_EQUAL(list[0], "A");
    TEST_ASSERT_EQUAL(list[1], "B");

    // Удаление активного (B) → активируется оставшийся (A).
    TEST_ASSERT_EQUAL(active_profile_name(dir.str()), "B");
    TEST_ASSERT_TRUE(delete_profile(dir.str(), "B"));
    TEST_ASSERT_EQUAL(active_profile_name(dir.str()), "A");

    list = list_profiles(dir.str());
    TEST_ASSERT_EQUAL(list.size(), std::size_t(1));
    TEST_ASSERT_EQUAL(list[0], "A");

    // Удаление последнего профиля запрещено.
    TEST_ASSERT_FALSE(delete_profile(dir.str(), "B"));
    TEST_ASSERT_EQUAL(list_profiles(dir.str()).size(), std::size_t(1));
}

static void test_profile_load_missing_returns_default() {
    TmpDir dir;
    const Config cfg = load_profile(dir.str(), "нет такого");
    TEST_ASSERT_EQUAL(cfg.sources.size(), std::size_t(1));
    TEST_ASSERT_EQUAL(cfg.schedule_minutes, 60);
}

static void test_profile_slug_sanitizes() {
    TEST_ASSERT_EQUAL(profile_slug("a b/c"), "a_b_c");
    TEST_ASSERT_EQUAL(profile_slug(""), "default");
    TEST_ASSERT_EQUAL(profile_slug("Profile-1"), "Profile-1");
    TEST_ASSERT_EQUAL(profile_slug("foo.bar"), "foo_bar");
}

} // namespace

REGISTER_TEST(test_profile_save_and_load_roundtrip);
REGISTER_TEST(test_profile_multiple_and_delete);
REGISTER_TEST(test_profile_load_missing_returns_default);
REGISTER_TEST(test_profile_slug_sanitizes);
