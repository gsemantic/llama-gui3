#include "project.h"
#include "engine.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;
namespace coder {

namespace {

bool run_capture(const std::string& cmd, std::string& out) {
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return false;
    char buf[4096];
    out.clear();
    while (fgets(buf, sizeof(buf), f)) out += buf;
    int rc = pclose(f);
    return rc == 0;
}

} // anonymous namespace

std::string setting_get_str(const std::string& key, const std::string& def) {
    auto& st = engine_state();
    /* Используем Engine callbacks если доступны, иначе — заглушка. */
    /* В текущей архитектуре Engine::init() ещё не вызван при загрузке настроек,
     * поэтому project_load_settings вызывается ПОСЛЕ init(). */
    return def;
}

void setting_set_str(const std::string& key, const std::string& value) {
    /* Аналогично — через Engine callbacks. */
}

void project_load_settings() {
    auto& st = engine_state();
    /* Настройки загружаются через Engine::load_settings() после init(). */
    /* Эта функция вызывается для обратной совместимости. */
}

void project_save_settings() {
    /* Сохраняется через Engine::save_settings(). */
}

void project_detect_php() {
    auto& st = engine_state();
    if (!st.php_bin.empty()) return;
    std::string out;
    if (run_capture("php -v", out) && !out.empty()) {
        st.php_bin = "php";
    }
}

std::string project_resolve(const std::string& rel) {
    const auto& st = engine_state();
    if (st.project_dir.empty()) return rel;
    if (rel.empty()) return st.project_dir;
    if (rel.size() > 0 && (rel[0] == '/' || rel.find(":") == 1)) return rel;
    std::string p = st.project_dir;
    if (!p.empty() && p.back() != '/') p += '/';
    p += rel;
    return p;
}

} // namespace coder
