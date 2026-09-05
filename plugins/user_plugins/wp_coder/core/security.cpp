#include "security.h"
#include <algorithm>
#include <sstream>

namespace coder {
namespace security {

bool is_path_safe(const std::string& path) {
    /* Проверяем на path traversal. */
    if (path.find("..") != std::string::npos) return false;
    /* Проверяем на null bytes. */
    if (path.find('\0') != std::string::npos) return false;
    return true;
}

bool is_project_dir_valid(const std::string& project_dir) {
    if (project_dir.empty()) return true;
    std::string normalized = project_dir;
    while (!normalized.empty() && normalized.back() == '/')
        normalized.pop_back();
    if (normalized.empty()) normalized = "/";
    /* Запрещаем только ТОЧНЫЕ пути к критическим директориям. */
    static const std::vector<std::string> forbidden = {
        "/",
        "/bin",
        "/boot",
        "/dev",
        "/etc",
        "/lib",
        "/lib64",
        "/proc",
        "/root",
        "/sbin",
        "/sys",
        "/usr",
    };
    for (const auto& f : forbidden) {
        if (normalized == f) return false;
    }
    /* /var и /usr — разрешаем подкаталоги (например /var/www/). */
    return true;
}

bool is_path_not_dangerous(const std::string& abs_path) {
    /* Не разрешаем запись в критические системные директории. */
    static const std::vector<std::string> forbidden = {
        "/etc/passwd",
        "/etc/shadow",
        "/etc/sudoers",
        "/boot/",
        "/proc/",
        "/sys/",
        "/dev/"
    };
    for (const auto& f : forbidden) {
        if (abs_path.find(f) == 0) return false;
    }
    return true;
}

const std::vector<std::string>& blocked_commands() {
    static const std::vector<std::string> blocked = {
        "rm -rf /",
        "rm -rf /*",
        "mkfs",
        "dd if=",
        "> /dev/sda",
        ":(){ :|:& };:",  // fork bomb
        "chmod -R 777 /",
        "wget ",  // + pipe to sh
        "curl ",  // + pipe to sh
    };
    return blocked;
}

bool is_command_allowed(const std::string& cmd) {
    for (const auto& b : blocked_commands()) {
        if (cmd.find(b) != std::string::npos) return false;
    }
    return true;
}

std::string shell_escape(const std::string& s) {
    std::ostringstream out;
    out << "'";
    for (char c : s) {
        if (c == '\'') out << "'\\''";
        else out << c;
    }
    out << "'";
    return out.str();
}

} // namespace security
} // namespace coder
