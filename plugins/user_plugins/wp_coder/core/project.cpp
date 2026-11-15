#include "project.h"
#include "engine.h"
#include "shell.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;
namespace coder {

void project_detect_php() {
    auto& st = engine_state();
    if (!st.php_bin.empty()) return;
    std::string out;
    int rc = -1;
    if (shell::run_capture_status("php -v", out, rc, 30) && !out.empty()) {
        st.php_bin = "php";
    }
}

std::string project_resolve(const std::string& rel) {
    const auto& st = engine_state();
    if (st.project_dir.empty()) return rel;
    if (rel.empty()) return st.project_dir;

    /* Абсолютный путь — POSIX (/) либо Windows-диск (C:/, C:\, C:).
     *
     * Раньше проверка была `rel.find(":") == 1`, то есть ЛЮБОЙ путь с
     * двоеточием на второй позиции считался абсолютным и возвращался
     * как есть. На Linux строка вида "a:b/c" или "wp:content/x"
     * выходила из project_dir мимо всех проверок — минуя и
     * is_path_outside, и PermissionGate.
     *
     * Диском считается только буква + ':' + разделитель (или ровно "C:").
     * Иначе "a:b/c" — относительный путь с двоеточием в имени. */
    bool is_abs = rel[0] == '/' || rel[0] == '\\';
    if (!is_abs && rel.size() >= 2 && rel[1] == ':' &&
        std::isalpha(static_cast<unsigned char>(rel[0]))) {
        if (rel.size() == 2 || rel[2] == '/' || rel[2] == '\\') is_abs = true;
    }
    if (is_abs) return rel;

    std::string p = st.project_dir;
    if (!p.empty() && p.back() != '/' && p.back() != '\\') p += '/';
    p += rel;
    return p;
}

std::string project_detect_module(const std::string& project_dir) {
    if (project_dir.empty()) return "";
    std::error_code ec;
    const fs::path root(project_dir);
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) return "";

    const auto has_file = [&](const char* name) {
        std::error_code e;
        return fs::exists(root / name, e) && fs::is_regular_file(root / name, e);
    };
    const auto has_dir = [&](const char* name) {
        std::error_code e;
        return fs::exists(root / name, e) && fs::is_directory(root / name, e);
    };

    /* WordPress узнаётся по ядру или по теме/плагину: wp-config.php есть
     * не всегда (вне корня), а wp-content есть в любом проекте. */
    if (has_file("wp-config.php") || has_dir("wp-content") ||
        has_dir("wp-includes")) {
        return "wordpress";
    }

    /* Python: файлы зависимостей или хотя бы один .py в корне. Один
     * .py — это уже проект (калькулятор, скрипт), и «generic» оставил бы
     * его без модуля и без Python-навыков. */
    if (has_file("requirements.txt") || has_file("pyproject.toml") ||
        has_file("setup.py") || has_file("Pipfile") ||
        has_file("manage.py")) {
        return "python";
    }
    {
        std::error_code it_ec;
        for (fs::directory_iterator it(root, it_ec), end; it != end;
             it.increment(it_ec)) {
            if (it_ec) break;
            std::error_code file_ec;
            if (it->is_regular_file(file_ec) &&
                it->path().extension() == ".py") {
                return "python";
            }
        }
    }

    /* Остальные стеки модулей не имеют: "generic" — это не «проекта нет»,
     * а «нет доменного модуля». Devops — последним: Dockerfile/systemd-юниты
     * живут рядом с любым кодом, и «devops» не должен перебивать язык
     * проекта. */
    if (has_file("Dockerfile") || has_file("docker-compose.yml") ||
        has_file("docker-compose.yaml")) {
        return "devops";
    }

    return "generic";
}

} // namespace coder
