#include "dotenv.h"

#include <fstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>

namespace news_rewriter {

namespace {

std::string trim_env(const std::string& s) {
    const std::size_t b = s.find_first_not_of(" \t\r\n\"'");
    if (b == std::string::npos) return "";
    const std::size_t e = s.find_last_not_of(" \t\r\n\"'");
    return s.substr(b, e - b + 1);
}

void ensure_dir(const std::string& path) {
    const std::size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return;
    const std::string dir = path.substr(0, pos);
    if (dir.empty()) return;
    mkdir(dir.c_str(), 0700);
}

} // namespace

std::string dotenv_read(const std::string& path, const std::string& key) {
    std::ifstream in(path);
    if (!in) return "";
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (trim_env(line.substr(0, eq)) != key) continue;
        return trim_env(line.substr(eq + 1));
    }
    return "";
}

void dotenv_write(const std::string& path, const std::string& key,
                  const std::string& value) {
    ensure_dir(path);
    std::vector<std::string> lines;
    bool found = false;
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            const std::size_t eq = line.find('=');
            if (eq != std::string::npos &&
                trim_env(line.substr(0, eq)) == key) {
                lines.push_back(key + "=" + value);
                found = true;
            } else {
                lines.push_back(line);
            }
        }
    }
    if (!found) lines.push_back(key + "=" + value);
    std::ofstream out(path, std::ios::trunc);
    for (const auto& l : lines) out << l << "\n";
}

void dotenv_remove(const std::string& path, const std::string& key) {
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            const std::size_t eq = line.find('=');
            if (eq != std::string::npos &&
                trim_env(line.substr(0, eq)) == key) {
                continue;   // пропускаем удаляемый ключ
            }
            lines.push_back(line);
        }
    }
    std::ofstream out(path, std::ios::trunc);
    for (const auto& l : lines) out << l << "\n";
}

} // namespace news_rewriter
