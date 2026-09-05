#include "git_tools.h"
#include "tools_registry.h"
#include "engine.h"

#include <cstdio>
#include <string>
#include <sstream>

namespace coder {

namespace {

std::string run_capture(const std::string& cmd) {
    std::string full = cmd + " 2>&1";
    FILE* f = popen(full.c_str(), "r");
    if (!f) return "[ошибка] не удалось запустить: " + cmd;
    char buf[4096];
    std::string out;
    while (fgets(buf, sizeof(buf), f)) out += buf;
    pclose(f);
    return out;
}

std::string git_status() {
    const auto& dir = engine_state().project_dir;
    if (dir.empty()) return "[ошибка] не задан project_dir";
    std::string out = run_capture("cd \"" + dir + "\" && git status --short");
    return out.empty() ? "[git status: чисто — нет изменений]" : "[git status]:\n" + out;
}

std::string git_diff(const std::string& path) {
    const auto& dir = engine_state().project_dir;
    if (dir.empty()) return "[ошибка] не задан project_dir";
    std::string cmd = "cd \"" + dir + "\" && git diff";
    if (!path.empty()) cmd += " -- " + path;
    std::string out = run_capture(cmd);
    if (out.size() > 8000) { out.resize(8000); out += "\n...[обрезано]"; }
    return out.empty() ? "[git diff: нет изменений]" : "[git diff]:\n" + out;
}

std::string git_log(int n) {
    const auto& dir = engine_state().project_dir;
    if (dir.empty()) return "[ошибка] не задан project_dir";
    if (n <= 0) n = 10;
    std::string out = run_capture("cd \"" + dir + "\" && git log --oneline -" + std::to_string(n));
    return out.empty() ? "[git log: нет коммитов]" : "[git log]:\n" + out;
}

std::string git_commit(const std::string& message) {
    const auto& dir = engine_state().project_dir;
    if (dir.empty()) return "[ошибка] не задан project_dir";
    if (message.empty()) return "[ошибка] пустое сообщение коммита";
    std::string out = run_capture("cd \"" + dir + "\" && git add -A && git commit -m \"" + message + "\"");
    return "[git commit]: " + (out.empty() ? "успешно" : out);
}

} // anonymous namespace

void register_git_tools() {
    auto& reg = ToolsRegistry::instance();

    reg.register_tool("git_status", [](const ToolArgs&) -> std::string {
        return git_status();
    }, "Статус git");

    reg.register_tool("git_diff", [](const ToolArgs& a) -> std::string {
        return git_diff(a.path);
    }, "Разница с HEAD");

    reg.register_tool("git_log", [](const ToolArgs& a) -> std::string {
        return git_log(a.k);
    }, "История коммитов");

    reg.register_tool("git_commit", [](const ToolArgs& a) -> std::string {
        return git_commit(a.query);
    }, "Создание коммита");
}

} // namespace coder
