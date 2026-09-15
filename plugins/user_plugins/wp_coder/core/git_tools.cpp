#include "git_tools.h"
#include "tools_registry.h"
#include "engine.h"
#include "shell.h"

#include <cstdio>
#include <string>
#include <sstream>

namespace coder {

namespace {

std::string git_run(const std::string& args, unsigned timeout = 30) {
    const auto& dir = engine_state().project_dir;
    if (dir.empty()) return "[ошибка] не задан project_dir";
    std::string out = shell::run_capture(
        "cd " + shell::shell_quote(dir) + " && git " + args, timeout);
    if (out.size() > 8000) out = shell::cap(out, 8000);
    return out;
}

std::string git_status() {
    std::string out = git_run("status --short");
    return out.empty() ? "[git status: чисто — нет изменений]" : "[git status]:\n" + out;
}

std::string git_diff(const std::string& path) {
    std::string args = "diff";
    if (!path.empty()) args += " -- " + shell::shell_quote(path);
    std::string out = git_run(args);
    return out.empty() ? "[git diff: нет изменений]" : "[git diff]:\n" + out;
}

std::string git_log(int n) {
    if (n <= 0) n = 10;
    std::string out = git_run("log --oneline -" + std::to_string(n));
    return out.empty() ? "[git log: нет коммитов]" : "[git log]:\n" + out;
}

/* 3.4: частичное индексирование (git add <path> вместо git add -A). */
std::string git_add(const std::string& path) {
    std::string args = "add";
    if (!path.empty()) args += " " + shell::shell_quote(path);
    else args += " -A";
    std::string out = git_run(args);
    if (out.empty()) return "[git add]: " + (path.empty() ? std::string("все изменения") : path);
    return "[git add]: " + shell::cap(out, 4000);
}

/* 3.4: список веток (QUERY пуст) или создание новой (QUERY = имя). */
std::string git_branch(const std::string& name) {
    if (name.empty()) {
        std::string out = git_run("branch");
        return out.empty() ? "[git branch: пусто]" : out;
    }
    std::string out = git_run("branch " + shell::shell_quote(name));
    return "[git branch]: создана " + name
           + (out.empty() ? "" : "\n" + shell::cap(out, 4000));
}

/* 3.4: переключение ветки. */
std::string git_checkout(const std::string& branch) {
    if (branch.empty()) return "[ошибка] укажи ветку (QUERY)";
    std::string out = git_run("checkout " + shell::shell_quote(branch));
    return "[git checkout]: " + (out.empty() ? branch : shell::cap(out, 4000));
}

std::string git_commit(const std::string& message, const std::string& path) {
    if (message.empty()) return "[ошибка] пустое сообщение коммита";
    /* Частичные коммиты: если задан PATH — индексируем только его. */
    std::string out = git_run(
        "add " + (path.empty() ? std::string("-A") : shell::shell_quote(path))
        + " && git commit -m " + shell::shell_quote(message), 60);
    return "[git commit]: " + (out.empty() ? "успешно" : shell::cap(out, 4000));
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
        return git_commit(a.query, a.path);
    }, "Создание коммита (PATH — частичный)");

    reg.register_tool("git_add", [](const ToolArgs& a) -> std::string {
        return git_add(a.path);
    }, "Индексация изменений (PATH — частичная)");

    reg.register_tool("git_branch", [](const ToolArgs& a) -> std::string {
        return git_branch(a.query);
    }, "Список/создание веток");

    reg.register_tool("git_checkout", [](const ToolArgs& a) -> std::string {
        return git_checkout(a.query);
    }, "Переключение ветки");
}

} // namespace coder