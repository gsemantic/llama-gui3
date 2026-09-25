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

} // namespace coder
