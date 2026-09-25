#include "devops_tools.h"
#include "../../core/tool.h"
#include "../../core/tools_registry.h"
#include "../../core/engine.h"
#include "../../core/shell.h"
#include "../../core/security.h"

#include <cstdio>
#include <sstream>
#include <iostream>
#include <algorithm>

namespace coder {
namespace devops {

namespace {

constexpr size_t kDevopsMaxOutput = 10000;

/* И1.3: чтение типизированных аргументов. */
std::string arg_str(const json::JsonValue& a, const char* key) {
    return a.get_string(key);
}

ToolOutput out(std::string title, std::string text) {
    ToolOutput o;
    o.title = std::move(title);
    o.output = std::move(text);
    return o;
}

/* Ошибка: title не нужен, текст уходит модели как есть. */
ToolOutput out(std::string text) {
    ToolOutput o;
    o.output = std::move(text);
    return o;
}

} // anonymous namespace

void register_devops_tools() {
    auto& reg = ToolsRegistry::instance();

    /* --- Docker --- */
    {
        ToolDef def;
        def.name = "docker_build";
        def.description = "Сборка Docker-образа";
        /* Dockerfile может содержать произвольные RUN-шаги, то есть
         * выполнить код с побочными эффектами. */
        def.flags = TF_EXECUTES | TF_DESTRUCTIVE | TF_SLOW;
        def.permission_key = "docker";
        SchemaBuilder b;
        b.str("path", "путь к Dockerfile; пусто = Dockerfile по умолчанию")
         .str("query", "контекст сборки; пусто = текущий каталог");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            std::string ctx = arg_str(a, "query");
            ctx = ctx.empty() ? "." : shell::shell_quote(ctx);
            std::string file = arg_str(a, "path");
            file = file.empty() ? "" : " -f " + shell::shell_quote(file);
            std::string result = shell::run_capture("docker build" + file + " " + ctx, 300);
            return out("docker build", result.empty() ? "[docker build: нет вывода]"
                                                       : shell::cap(result, kDevopsMaxOutput));
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "docker_run";
        def.description = "Запуск Docker-контейнера";
        def.flags = TF_EXECUTES | TF_DESTRUCTIVE | TF_SLOW;
        def.permission_key = "docker";
        SchemaBuilder b;
        b.str("cli", "аргументы docker run, например «-d -p 8080:80 nginx»")
         .required("cli");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            std::string cli = arg_str(a, "cli");
            if (cli.empty()) return out("[ошибка] укажи параметры запуска (cli)");
            /* docker run args сложны: --name, -p, -v и т.д.
             * Полное экранирование сломает синтаксис Docker.
             * Валидируем: запрещаем shell-метасимволы ; | & ` $ > <. */
            for (char c : cli) {
                if (c == ';' || c == '|' || c == '&' || c == '`' ||
                    c == '$' || c == '>' || c == '<') {
                    return out("[запрещено] shell-инъекция в docker run");
                }
            }
            std::string result = shell::run_capture("docker run " + cli, 120);
            return out("docker run", result.empty() ? "[docker run: нет вывода]"
                                                    : shell::cap(result, kDevopsMaxOutput));
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "docker_ps";
        def.description = "Список Docker-контейнеров";
        def.flags = TF_READ_ONLY | TF_EXECUTES;
        def.permission_key = "read";
        SchemaBuilder b;
        b.str("query", "фильтр docker, например status=running");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            std::string q = arg_str(a, "query");
            std::string filter = q.empty() ? "" : " --filter " + shell::shell_quote(q);
            std::string result = shell::run_capture(
                "docker ps" + filter + " --format 'table {{.Names}}\\t{{.Status}}\\t{{.Ports}}'", 30);
            return out("docker ps", result.empty() ? "[docker ps: нет контейнеров]"
                                                   : shell::cap(result, kDevopsMaxOutput));
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "docker_logs";
        def.description = "Логи Docker-контейнера (последние 100 строк)";
        def.flags = TF_READ_ONLY | TF_EXECUTES;
        def.permission_key = "read";
        SchemaBuilder b;
        b.str("query", "имя контейнера").required("query");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            std::string name = security::sanitize_ident(arg_str(a, "query"));
            if (name.empty()) return out("[ошибка] невалидное имя контейнера");
            std::string result = shell::run_capture(
                "docker logs --tail 100 " + shell::shell_quote(name), 60);
            return out("docker logs", result.empty() ? "[docker logs: пусто]"
                                                     : shell::cap(result, kDevopsMaxOutput));
        };
        reg.register_def(std::move(def));
    }

    /* --- Systemd --- */
    {
        ToolDef def;
        def.name = "systemd_status";
        def.description = "Статус systemd-сервиса";
        def.flags = TF_READ_ONLY | TF_EXECUTES;
        def.permission_key = "read";
        SchemaBuilder b;
        b.str("query", "имя сервиса, например nginx.service").required("query");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            std::string name = security::sanitize_ident(arg_str(a, "query"));
            if (name.empty()) return out("[ошибка] невалидное имя сервиса");
            std::string result = shell::run_capture(
                "systemctl status " + shell::shell_quote(name), 30);
            return out("systemctl", result.empty() ? "[systemd: нет вывода]"
                                                   : shell::cap(result, kDevopsMaxOutput));
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "systemd_restart";
        def.description = "Перезапуск systemd-сервиса (через sudo)";
        def.flags = TF_EXECUTES | TF_DESTRUCTIVE;
        def.permission_key = "systemd";
        SchemaBuilder b;
        b.str("query", "имя сервиса, например nginx.service").required("query");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            std::string name = security::sanitize_ident(arg_str(a, "query"));
            if (name.empty()) return out("[ошибка] невалидное имя сервиса");
            std::string result = shell::run_capture(
                "sudo systemctl restart " + shell::shell_quote(name), 60);
            return out("systemctl restart", result.empty() ? "[systemd restart: OK]"
                                                           : shell::cap(result, kDevopsMaxOutput));
        };
        reg.register_def(std::move(def));
    }

    /* --- Nginx --- */
    {
        ToolDef def;
        def.name = "nginx_test";
        def.description = "Проверка конфигурации Nginx (nginx -t)";
        def.flags = TF_READ_ONLY | TF_EXECUTES;
        def.permission_key = "read";
        SchemaBuilder b;
        b.str("query", "путь к конфигу; пусто = основной nginx.conf");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            std::string q = arg_str(a, "query");
            std::string cfg = q.empty() ? "" : " -c " + shell::shell_quote(q);
            std::string result = shell::run_capture("sudo nginx -t" + cfg, 30);
            return out("nginx -t", result.empty() ? "[nginx -t: OK]"
                                                  : shell::cap(result, kDevopsMaxOutput));
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "nginx_reload";
        def.description = "Перезагрузка Nginx (без даунтайма)";
        def.flags = TF_EXECUTES | TF_DESTRUCTIVE;
        def.permission_key = "systemd";
        def.parameters = SchemaBuilder().build();
        def.handler = [](const json::JsonValue&, ToolContext&) -> ToolOutput {
            std::string result = shell::run_capture("sudo nginx -s reload", 30);
            return out("nginx reload", result.empty() ? "[nginx reload: OK]"
                                                     : shell::cap(result, kDevopsMaxOutput));
        };
        reg.register_def(std::move(def));
    }

    /* --- Cron --- */
    {
        ToolDef def;
        def.name = "cron_list";
        def.description = "Список cron-задач";
        def.flags = TF_READ_ONLY | TF_EXECUTES;
        def.permission_key = "read";
        SchemaBuilder b;
        b.str("query", "пользователь; пусто = текущий");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            std::string q = arg_str(a, "query");
            std::string user = q.empty()
                ? "" : " -u " + shell::shell_quote(security::sanitize_ident(q));
            std::string result = shell::run_capture("crontab -l" + user, 30);
            return out("crontab", result.empty() ? "[crontab: пусто или нет доступа]"
                                                 : shell::cap(result, kDevopsMaxOutput));
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "cron_add";
        def.description = "Добавление cron-задачи";
        def.flags = TF_EXECUTES | TF_DESTRUCTIVE;
        def.permission_key = "cron";
        SchemaBuilder b;
        b.str("query", "cron-выражение, например */5 * * * *")
         .str("cli", "команда, которую нужно выполнять")
         .required("query").required("cli");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            std::string expr = arg_str(a, "query");
            std::string cmdline = arg_str(a, "cli");
            if (expr.empty() || cmdline.empty())
                return out("[ошибка] query=cron-выражение, cli=команда");
            /* Валидация cron expression: только цифры, пробелы, *, /, -, и запятые. */
            for (char c : expr) {
                if (!std::isdigit(static_cast<unsigned char>(c)) &&
                    c != ' ' && c != '*' && c != '/' && c != '-' && c != ',') {
                    return out("[запрещено] невалидное cron-выражение: " + expr);
                }
            }
            /* Экранируем команду для shell. */
            std::string entry = expr + " " + cmdline;
            std::string cmd = " (crontab -l 2>/dev/null; echo " +
                              shell::shell_quote(entry) + ") | crontab -";
            std::string result = shell::run_capture(cmd, 30);
            return out("cron", result.empty() ? "[cron: добавлено]"
                                              : shell::cap(result, kDevopsMaxOutput));
        };
        reg.register_def(std::move(def));
    }

    /* --- SSH --- */
    {
        ToolDef def;
        def.name = "ssh_exec";
        def.description = "Выполнение команды по SSH на удалённом хосте";
        def.flags = TF_EXECUTES | TF_DESTRUCTIVE | TF_NETWORK | TF_SLOW;
        def.permission_key = "ssh";
        SchemaBuilder b;
        b.str("query", "хост или user@host")
         .str("cli", "команда для выполнения")
         .required("query").required("cli");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            std::string raw_host = arg_str(a, "query");
            std::string host = security::sanitize_ident(raw_host);
            /* Допускаем user@host формат. */
            if (host.empty()) {
                for (char c : raw_host) {
                    if (!std::isalnum(static_cast<unsigned char>(c)) &&
                        c != '@' && c != '.' && c != '-' && c != ':') {
                        return out("[запрещено] невалидный хост: " + raw_host);
                    }
                }
                host = raw_host;
            }
            std::string cmd = "ssh " + shell::shell_quote(host) + " " +
                              shell::shell_quote(arg_str(a, "cli"));
            std::string result = shell::run_capture(cmd, 120);
            return out("ssh", result.empty() ? "[ssh: нет вывода]"
                                             : shell::cap(result, kDevopsMaxOutput));
        };
        reg.register_def(std::move(def));
    }
}

static const char* kDockerSkill =
    "# devops_docker\n"
    "Описание: Docker best practices\n"
    "- Multi-stage builds для уменьшения образа.\n"
    "- .dockerignore для исключения node_modules, .git.\n"
    "- HEALTHCHECK в Dockerfile.\n"
    "- Используй --init для корректного обработки сигналов.\n"
    "- Для продакшена: --restart unless-stopped.\n"
    "- Логи: docker logs --follow <container>.";

static const char* kSystemdSkill =
    "# devops_systemd\n"
    "Описание: systemd service management\n"
    "- Сервис: /etc/systemd/system/<name>.service\n"
    "- После правки: systemctl daemon-reload && systemctl restart <name>.\n"
    "- Автозапуск: systemctl enable <name>.\n"
    "- Логи: journalctl -u <name> -f.\n"
    "- Проверка: systemctl status <name>.";

static const char* kNginxSkill =
    "# devops_nginx\n"
    "Описание: Nginx конфигурация\n"
    "- Конфиги: /etc/nginx/sites-available/ + symlink в sites-enabled/.\n"
    "- Проверка: nginx -t.\n"
    "- Перезагрузка: nginx -s reload (без даунтайма).\n"
    "- SSL: certbot --nginx -d domain.com.\n"
    "- Проксирование: proxy_pass http://127.0.0.1:port;";

std::vector<Skill> get_devops_skills() {
    return {
        {"devops_docker", "Docker best practices", kDockerSkill, "devops"},
        {"devops_systemd", "systemd service management", kSystemdSkill, "devops"},
        {"devops_nginx", "Nginx конфигурация", kNginxSkill, "devops"}
    };
}

} // namespace devops
} // namespace coder
