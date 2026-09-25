#include "git_tools.h"
#include "tool.h"
#include "tools_registry.h"
#include "engine.h"
#include "shell.h"

#include <cstdio>
#include <string>
#include <sstream>

namespace coder {

namespace {

/* И1.3: короткие помощники для аргументов. */
std::string arg_str(const json::JsonValue& a, const char* key) {
    return a.get_string(key);
}

int arg_int(const json::JsonValue& a, const char* key) {
    return static_cast<int>(a.get_int(key, 0));
}

ToolOutput out(std::string title, std::string text) {
    ToolOutput o;
    o.title = std::move(title);
    o.output = std::move(text);
    return o;
}

std::string git_run(const std::string& args, const std::string& dir,
                    unsigned timeout = 30) {
    if (dir.empty()) return "[ошибка] не задан project_dir";
    /* git -C не требует cd: timeout(1) выполняет команду через execvp и
     * НЕ понимает встроенные команды shell («timeout: failed to run 'cd'»). */
    std::string out = shell::run_capture(
        "git -C " + shell::shell_quote(dir) + " " + args, timeout);
    if (out.size() > 8000) out = shell::cap(out, 8000);
    return out;
}

std::string git_status(const std::string& dir) {
    std::string result = git_run("status --short", dir);
    return result.empty() ? "[git status: чисто — нет изменений]"
                          : "[git status]:\n" + result;
}

std::string git_diff(const std::string& path, const std::string& dir) {
    std::string args = "diff";
    if (!path.empty()) args += " -- " + shell::shell_quote(path);
    std::string result = git_run(args, dir);
    return result.empty() ? "[git diff: нет изменений]" : "[git diff]:\n" + result;
}

std::string git_log(int n, const std::string& dir) {
    if (n <= 0) n = 10;
    std::string result = git_run("log --oneline -" + std::to_string(n), dir);
    return result.empty() ? "[git log: нет коммитов]" : "[git log]:\n" + result;
}

/* 3.4: частичное индексирование (git add <path> вместо git add -A). */
std::string git_add(const std::string& path, const std::string& dir) {
    std::string args = "add";
    if (!path.empty()) args += " " + shell::shell_quote(path);
    else args += " -A";
    std::string result = git_run(args, dir);
    if (result.empty())
        return "[git add]: " + (path.empty() ? std::string("все изменения") : path);
    return "[git add]: " + shell::cap(result, 4000);
}

/* 3.4: список веток (query пуст) или создание новой (query = имя). */
std::string git_branch(const std::string& name, const std::string& dir) {
    if (name.empty()) {
        std::string result = git_run("branch", dir);
        return result.empty() ? "[git branch: пусто]" : result;
    }
    std::string result = git_run("branch " + shell::shell_quote(name), dir);
    return "[git branch]: создана " + name
           + (result.empty() ? "" : "\n" + shell::cap(result, 4000));
}

/* 3.4: переключение ветки. */
std::string git_checkout(const std::string& branch, const std::string& dir) {
    if (branch.empty()) return "[ошибка] укажи ветку (query)";
    std::string result = git_run("checkout " + shell::shell_quote(branch), dir);
    return "[git checkout]: " + (result.empty() ? branch : shell::cap(result, 4000));
}

std::string git_commit(const std::string& message, const std::string& path,
                       const std::string& dir) {
    if (message.empty()) return "[ошибка] пустое сообщение коммита";
    /* Частичные коммиты: если задан path — индексируем только его.
     * add и commit раздельно: «timeout ... A && B» выполнит A под timeout,
     * а B — в шелле РОДИТЕЛЯ (без git -C). */
    git_run("add " + (path.empty() ? std::string("-A") : shell::shell_quote(path)), dir, 30);
    std::string result = git_run("commit -m " + shell::shell_quote(message), dir, 60);
    return "[git commit]: " + (result.empty() ? "успешно" : shell::cap(result, 4000));
}

} // anonymous namespace

void register_git_tools() {
    auto& reg = ToolsRegistry::instance();

    {
        ToolDef def;
        def.name = "git_status";
        def.description = "Статус рабочей копии git";
        def.flags = TF_READ_ONLY | TF_EXECUTES;
        def.permission_key = "read";
        def.parameters = SchemaBuilder().build();
        def.handler = [](const json::JsonValue&, ToolContext& ctx) -> ToolOutput {
            return out("git status", git_status(ctx.project_dir()));
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "git_diff";
        def.description = "Разница с HEAD";
        def.flags = TF_READ_ONLY | TF_EXECUTES;
        def.permission_key = "read";
        SchemaBuilder b;
        b.str("path", "файл или каталог; пусто = все изменения");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            return out("git diff", git_diff(arg_str(a, "path"), ctx.project_dir()));
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "git_log";
        def.description = "История коммитов";
        def.flags = TF_READ_ONLY | TF_EXECUTES;
        def.permission_key = "read";
        SchemaBuilder b;
        b.integer_range("k", "сколько коммитов показать (по умолчанию 10)", 1, 1000);
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            return out("git log", git_log(arg_int(a, "k"), ctx.project_dir()));
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "git_commit";
        def.description = "Создание коммита (path — частичный коммит)";
        /* Коммит меняет историю и не откатывается этим инструментом. */
        def.flags = TF_EXECUTES | TF_DESTRUCTIVE;
        def.permission_key = "git";
        SchemaBuilder b;
        b.str("query", "сообщение коммита")
         .str("path", "файл для частичного коммита; пусто = все изменения")
         .required("query");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            return out("git commit", git_commit(arg_str(a, "query"),
                                                arg_str(a, "path"), ctx.project_dir()));
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "git_add";
        def.description = "Индексация изменений (path — частичная)";
        /* Индексация меняет состояние репозитория, и отменить её этим
         * инструментом нельзя (нужен git reset). */
        def.flags = TF_EXECUTES | TF_DESTRUCTIVE;
        def.permission_key = "git";
        SchemaBuilder b;
        b.str("path", "файл или каталог; пусто = все изменения (-A)");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            return out("git add", git_add(arg_str(a, "path"), ctx.project_dir()));
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "git_branch";
        def.description = "Список веток (query пуст) или создание новой";
        /* Создание ветки — изменение репозитория, откатить его
         * инструмент не умеет. */
        def.flags = TF_EXECUTES | TF_DESTRUCTIVE;
        def.permission_key = "git";
        SchemaBuilder b;
        b.str("query", "имя новой ветки; пусто = показать список");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            return out("git branch", git_branch(arg_str(a, "query"), ctx.project_dir()));
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "git_checkout";
        def.description = "Переключение ветки";
        /* Переключение ветки способно выбросить незакоммиченные
         * изменения — откатить это автоматически нельзя. */
        def.flags = TF_EXECUTES | TF_DESTRUCTIVE;
        def.permission_key = "git";
        SchemaBuilder b;
        b.str("query", "имя ветки").required("query");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            return out("git checkout", git_checkout(arg_str(a, "query"), ctx.project_dir()));
        };
        reg.register_def(std::move(def));
    }
}

} // namespace coder