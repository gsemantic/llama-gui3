/*
 * test_manifest_consistency.cpp — И0.8: версия и манифесты.
 *
 * Версия плагина была продублирована в четырёх файлах (plugin.json,
 * plugins/wp_coder.json, ll_plugin_info(), CHANGELOG.md) и разошлась:
 * 0.3.0 / 0.1.0 / 0.3.0 / 0.4.0. Теперь источник истины — plugin.json,
 * а CMake извлекает из него WP_CODER_VERSION. Тест ловит возврат к
 * копированию версии в коде.
 *
 * Здесь же проверка, что удалённые мёртвые файлы действительно удалены
 * и не вернулись в дерево (см. src/README.md).
 */

#include "test_framework.h"
#include "../core/limits.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace coder;

namespace {

/* Корень исходников плагина: tests/ -> ../ */
fs::path plugin_root() {
    return fs::path(__FILE__).parent_path().parent_path();
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/* Грубое, но достаточное извлечение значения JSON-поля. */
std::string json_field(const std::string& text, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t p = text.find(pat);
    if (p == std::string::npos) return "";
    size_t colon = text.find(':', p + pat.size());
    if (colon == std::string::npos) return "";
    size_t q1 = text.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    size_t q2 = text.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return text.substr(q1 + 1, q2 - q1 - 1);
}

}  // namespace

TEST(manifest_plugin_json_has_version) {
    std::string j = read_file(plugin_root() / "plugin.json");
    ASSERT_TRUE(!j.empty());
    std::string v = json_field(j, "version");
    ASSERT_TRUE(!v.empty());
    ASSERT_TRUE(v == "0.6.0");
}

TEST(manifest_repo_copy_matches_plugin_json) {
    /* plugins/wp_coder.json обязан совпадать с plugin.json: хост ищет
     * манифест рядом с .so, а в дереве репозитория лежит копия. */
    fs::path a = plugin_root() / "plugin.json";
    fs::path b = plugin_root().parent_path().parent_path().parent_path() / "wp_coder.json";
    if (!fs::exists(b)) return;  /* сборка плагина отдельно от репозитория */
    ASSERT_TRUE(read_file(a) == read_file(b));
}

TEST(manifest_capabilities_include_all_modules) {
    std::string j = read_file(plugin_root() / "plugin.json");
    /* Раньше корневая копия была на 0.1.0 и не знала про python/devops. */
    for (const char* cap : {"code_generation", "wordpress", "python", "devops",
                            "rag", "ui_window", "agent", "modules"}) {
        ASSERT_TRUE(j.find(cap) != std::string::npos);
    }
}

TEST(manifest_api_version_matches_sdk) {
    std::string j = read_file(plugin_root() / "plugin.json");
    /* plugin_api.h: LLAMA_PLUGIN_API_VERSION_MAJOR/MINOR/PATCH = 1/0/0.
     * PluginManager отвергает плагин при несовпадении строки. */
    ASSERT_TRUE(json_field(j, "api_version") == "1.0.0");
}

/* Версия в коде берётся из CMake-макроса. Если макрос не пришёл —
 * используется запасное значение; и то и другое должно совпадать с
 * plugin.json, иначе хост покажет в UI другую версию, чем в манифесте. */
TEST(manifest_code_version_matches_manifest) {
#ifdef WP_CODER_VERSION
    const char* code_version = WP_CODER_VERSION;
#else
    const char* code_version = "0.6.0";
#endif
    std::string j = read_file(plugin_root() / "plugin.json");
    ASSERT_TRUE(json_field(j, "version") == std::string(code_version));
}

TEST(manifest_changelog_documents_current_version) {
    std::string c = read_file(plugin_root() / "CHANGELOG.md");
    ASSERT_TRUE(!c.empty());
    ASSERT_TRUE(c.find("## [0.6.0]") != std::string::npos);
    ASSERT_TRUE(c.find("## [0.5.0]") != std::string::npos);
    /* Старая версия 0.4.0 осталась в истории — это нормально. */
    ASSERT_TRUE(c.find("## [0.4.0]") != std::string::npos);
}

/* --- Мёртвый код не должен вернуться (И0.6) --- */

TEST(dead_agent_files_are_gone) {
    const char* gone[] = {
        "src/wp_coder_orchestrator.h", "src/wp_coder_orchestrator.cpp",
        "src/wp_coder_plugin.cpp",
        "src/wp_theme_agent.h",  "src/wp_theme_agent.cpp",
        "src/wp_plugin_agent.h", "src/wp_plugin_agent.cpp",
        "src/wp_hook_agent.h",   "src/wp_hook_agent.cpp",
        "src/wp_deploy_agent.h", "src/wp_deploy_agent.cpp",
        "src/wp_rag_agent.h",    "src/wp_rag_agent.cpp",
        "src/wp_terminal_agent.h", "src/wp_terminal_agent.cpp",
        "src/wp_file_agent.h",   "src/wp_file_agent.cpp",
        "tests/test_wp_theme_agent.cpp", "tests/test_wp_deploy_agent.cpp",
    };
    for (const char* rel : gone) {
        ASSERT_TRUE(!fs::exists(plugin_root() / rel));
    }
}

TEST(src_dir_contains_only_entry_point_and_readme) {
    /* В src/ должен быть только plugin_main.cpp (и README.md, который
     * объясняет, почему больше ничего быть не должно). */
    int cpp_count = 0;
    for (const auto& e : fs::directory_iterator(plugin_root() / "src")) {
        if (e.is_regular_file() && e.path().extension() == ".cpp") ++cpp_count;
    }
    ASSERT_EQ(cpp_count, 1);
}

TEST(no_removed_symbols_in_sources) {
    /* Удалённый код ссылался на несуществующий ABI. Эти имена не должны
     * появляться снова: их нет ни в одном заголовке проекта. */
    const char* banned[] = {
        "AGENT_PLUGIN_API_VERSION", "PluginExports",
        "plugin_create_agent", "AgentContextMock",
    };
    /* Сам тест эти строки содержит по необходимости — себя не сканируем
     * (сравниваем по имени файла: __FILE__ может быть относительным). */
    const std::string self_name = "test_manifest_consistency.cpp";
    for (const auto& e : fs::recursive_directory_iterator(plugin_root())) {
        if (!e.is_regular_file()) continue;
        if (e.path().filename().string() == self_name) continue;
        auto ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".h") continue;
        std::string s = read_file(e.path());
        for (const char* b : banned) {
            if (s.find(b) != std::string::npos) {
                std::cerr << "  найдено запрещённое имя '" << b
                          << "' в " << e.path().string() << std::endl;
                ASSERT_TRUE(false);
            }
        }
    }
}

/* ======================================================================
 * И1.10 — манифест в build/plugins обязан называться по имени плагина.
 *
 * PluginManager::find_manifest_path() (src/plugins/plugin_manager.cpp)
 * пробует кандидатов по порядку: lib<name>.json, <name>.json,
 * <name>.plugin.json, и только потом общий plugin.json. Отсюда две
 * беды, обе были в дереве:
 *
 *   D20 — wp_coder копировал plugin.json, но его перехватывал
 *         кандидат №2: хост читал бросившийся wp_coder.json версии
 *         0.1.0 без capabilities python/devops. Правка «скопировать
 *         plugin.json» не работала.
 *   D21 — общий plugin.json в каталоге с несколькими плагинами
 *         принадлежит сразу всем: hello_plugin (писал туда же с
 *         08-06) и wp_coder перетирали друг друга, и кто последний
 *         записал — тот и «владелец». В 08-26 это давало wp_coder
 *         версию 0.1.0, в 09-25 — hello_plugin capabilities wp_coder.
 *
 * Имя файла манифеста = имя плагина. Общее имя недопустимо.
 * ====================================================================== */

TEST(build_manifest_is_named_after_plugin) {
#ifndef WP_CODER_BUILD_PLUGIN_DIR
    /* Тесты собраны вне основного CMake — проверять нечего. */
    return;
#else
    fs::path dir(WP_CODER_BUILD_PLUGIN_DIR);
    fs::path own = dir / "wp_coder.json";
    if (!fs::exists(own)) {
        std::cerr << "  нет манифеста " << own.string() << std::endl;
        ASSERT_TRUE(false);
    }
    /* Содержимое совпадает с единственным источником истины. */
    ASSERT_TRUE(read_file(own) == read_file(plugin_root() / "plugin.json"));
    /* Имя плагина внутри совпадает с именем, из которого выведен файл:
     * иначе PluginManager отбросит манифест как чужой. */
    ASSERT_EQ(json_field(read_file(own), "name"), std::string("wp_coder"));
#endif
}

TEST(build_dir_has_no_shared_plugin_json) {
#ifndef WP_CODER_BUILD_PLUGIN_DIR
    return;
#else
    /* Общий plugin.json в каталоге с несколькими плагинами — источник
     * перекрёстных прав и версий. Его здесь быть не должно. */
    fs::path shared = fs::path(WP_CODER_BUILD_PLUGIN_DIR) / "plugin.json";
    if (fs::exists(shared)) {
        std::cerr << "  найден общий манифест " << shared.string() << std::endl;
        ASSERT_TRUE(false);
    }
#endif
}

TEST(cmake_does_not_copy_manifest_under_shared_name) {
    /* Проверяем намерение в CMakeLists: destination обязан быть
     * wp_coder.json, а не plugin.json.
     *
     * Путь и команда стоят в разных строках блока POST_BUILD, поэтому
     * «rm -f» ищем не в той же строке, а в тексте от предыдущего
     * COMMAND до самого вхождения: иначе проверка пропустила бы и
     * копирование, и очистку. */
    std::string cml = read_file(plugin_root() / "CMakeLists.txt");
    size_t pos = 0;
    while ((pos = cml.find("/plugins/plugin.json", pos)) != std::string::npos) {
        size_t cmd = cml.rfind("COMMAND", pos);
        std::string block = (cmd == std::string::npos)
            ? std::string()
            : cml.substr(cmd, pos - cmd);
        if (block.find("rm -f") == std::string::npos) {
            std::cerr << "  CMakeLists копирует манифест в общий plugin.json: "
                      << block << std::endl;
            ASSERT_TRUE(false);
        }
        pos += 8;
    }
    /* Собственный манифест копируется обязательно. */
    ASSERT_TRUE(cml.find("/plugins/wp_coder.json") != std::string::npos);
}

TEST(host_rejects_manifest_of_another_plugin) {
    /* Сторона хоста: манифест с чужим именем должен ОТБРАСЫВАТЬСЯ, а не
     * применяться с предупреждением. Иначе И2 (система разрешений)
     * построит права на чужом манифесте. */
    std::string pm = read_file(plugin_root().parent_path().parent_path()
                              / "src" / "plugins" / "plugin_manager.cpp");
    if (pm.empty()) return;   /* плагин собирается отдельно от репозитория */
    size_t at = pm.find("does not belong to plugin");
    ASSERT_TRUE(at != std::string::npos);
    /* Рядом обязателен сброс манифеста, а не только сообщение. */
    size_t reset = pm.find("manifest = PluginManifest{}", at);
    ASSERT_TRUE(reset != std::string::npos);
}

TEST(root_cmake_guards_against_stale_test_artifacts) {
    /* D22: CMake не удаляет сгенерированные файлы подкаталога, который
     * перестал конфигурироваться. При BUILD_TESTS=OFF старые бинарники
     * остаются, и ctest отвечает за них «Passed» — зелёный свет в пустоту.
     * Конфигурация обязана падать, а не молчать. */
    fs::path root = plugin_root().parent_path().parent_path().parent_path();
    std::string cml = read_file(root / "CMakeLists.txt");
    if (cml.empty()) return;
    ASSERT_TRUE(cml.find("BUILD_TESTS=OFF") != std::string::npos);
    ASSERT_TRUE(cml.find("tests/CTestTestfile.cmake") != std::string::npos);
    ASSERT_TRUE(cml.find("FATAL_ERROR") != std::string::npos);
}

/* --- Проектные строки-инварианты, которые легко сломать --- */

TEST(limits_file_is_single_source_of_truth) {
    /* Лимиты объявлены в core/limits.h и НЕ должны дублироваться
     * числами в коде инструментов. */
    std::string lim = read_file(plugin_root() / "core" / "limits.h");
    ASSERT_TRUE(!lim.empty());
    ASSERT_TRUE(lim.find("kMaxToolOutput") != std::string::npos);
    ASSERT_TRUE(lim.find("kResultBudget") != std::string::npos);
    /* Символические константы равны ожидаемым значениям. */
    ASSERT_EQ((int)limits::kMaxToolOutput, 12000);
    ASSERT_EQ((int)limits::kResultBudget, 1500);
    ASSERT_EQ((int)limits::kMaxGrepMatches, 200);
    ASSERT_EQ((int)limits::kMaxSteps, 12);
    ASSERT_EQ((int)limits::kSessionBudget, 60000);
}
