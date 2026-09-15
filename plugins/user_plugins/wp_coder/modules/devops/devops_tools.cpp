#include "devops_tools.h"
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

} // anonymous namespace

void register_devops_tools() {
    auto& reg = ToolsRegistry::instance();

    /* Docker. */
    reg.register_tool("docker_build", [](const ToolArgs& a) -> std::string {
        std::string ctx = a.query.empty() ? "." : shell::shell_quote(a.query);
        std::string file = a.path.empty() ? "" : " -f " + shell::shell_quote(a.path);
        std::string out = shell::run_capture("docker build" + file + " " + ctx, 300);
        return out.empty() ? "[docker build: нет вывода]" : shell::cap(out, kDevopsMaxOutput);
    }, "Сборка Docker-образа");

    reg.register_tool("docker_run", [](const ToolArgs& a) -> std::string {
        if (a.cli.empty()) return "[ошибка] укажи параметры запуска (CLI)";
        /* docker run args сложны: --name, -p, -v и т.д.
         * Полное экранирование сломает синтаксис Docker.
         * Валидируем: запрещаем ; | && ` $() и другие shell-метасимволы. */
        for (size_t i = 0; i < a.cli.size(); ++i) {
            char c = a.cli[i];
            if (c == ';' || c == '|' || c == '`' || c == '$') {
                if (i + 1 < a.cli.size() && a.cli[i + 1] == '(')
                    return "[запрещено] shell-инъекция в docker run";
            }
        }
        std::string out = shell::run_capture("docker run " + a.cli, 120);
        return out.empty() ? "[docker run: нет вывода]" : shell::cap(out, kDevopsMaxOutput);
    }, "Запуск Docker-контейнера");

    reg.register_tool("docker_ps", [](const ToolArgs& a) -> std::string {
        std::string filter = a.query.empty() ? "" : " --filter " + shell::shell_quote(a.query);
        std::string out = shell::run_capture(
            "docker ps" + filter + " --format 'table {{.Names}}\\t{{.Status}}\\t{{.Ports}}'", 30);
        return out.empty() ? "[docker ps: нет контейнеров]" : shell::cap(out, kDevopsMaxOutput);
    }, "Список Docker-контейнеров");

    reg.register_tool("docker_logs", [](const ToolArgs& a) -> std::string {
        if (a.query.empty()) return "[ошибка] укажи имя контейнера (QUERY)";
        std::string name = security::sanitize_ident(a.query);
        if (name.empty()) return "[ошибка] невалидное имя контейнера";
        std::string out = shell::run_capture("docker logs --tail 100 " + shell::shell_quote(name), 60);
        return out.empty() ? "[docker logs: пусто]" : shell::cap(out, kDevopsMaxOutput);
    }, "Логи Docker-контейнера");

    /* Systemd. */
    reg.register_tool("systemd_status", [](const ToolArgs& a) -> std::string {
        if (a.query.empty()) return "[ошибка] укажи имя сервиса (QUERY)";
        std::string name = security::sanitize_ident(a.query);
        if (name.empty()) return "[ошибка] невалидное имя сервиса";
        std::string out = shell::run_capture("systemctl status " + shell::shell_quote(name), 30);
        return out.empty() ? "[systemd: нет вывода]" : shell::cap(out, kDevopsMaxOutput);
    }, "Статус systemd-сервиса");

    reg.register_tool("systemd_restart", [](const ToolArgs& a) -> std::string {
        if (a.query.empty()) return "[ошибка] укажи имя сервиса (QUERY)";
        std::string name = security::sanitize_ident(a.query);
        if (name.empty()) return "[ошибка] невалидное имя сервиса";
        std::string out = shell::run_capture("sudo systemctl restart " + shell::shell_quote(name), 60);
        return out.empty() ? "[systemd restart: OK]" : shell::cap(out, kDevopsMaxOutput);
    }, "Перезапуск systemd-сервиса");

    /* Nginx. */
    reg.register_tool("nginx_test", [](const ToolArgs& a) -> std::string {
        std::string cfg = a.query.empty() ? "" : " -c " + shell::shell_quote(a.query);
        std::string out = shell::run_capture("sudo nginx -t" + cfg, 30);
        return out.empty() ? "[nginx -t: OK]" : shell::cap(out, kDevopsMaxOutput);
    }, "Проверка конфигурации Nginx");

    reg.register_tool("nginx_reload", [](const ToolArgs&) -> std::string {
        std::string out = shell::run_capture("sudo nginx -s reload", 30);
        return out.empty() ? "[nginx reload: OK]" : shell::cap(out, kDevopsMaxOutput);
    }, "Перезагрузка Nginx");

    /* Cron. */
    reg.register_tool("cron_list", [](const ToolArgs& a) -> std::string {
        std::string user = a.query.empty() ? "" : " -u " + shell::shell_quote(security::sanitize_ident(a.query));
        std::string out = shell::run_capture("crontab -l" + user, 30);
        return out.empty() ? "[crontab: пусто или нет доступа]" : shell::cap(out, kDevopsMaxOutput);
    }, "Список cron-задач");

    reg.register_tool("cron_add", [](const ToolArgs& a) -> std::string {
        if (a.query.empty() || a.cli.empty())
            return "[ошибка] QUERY=cron_expression, CLI=команда";
        /* Валидация cron expression: только цифры, пробелы, *, /, -, и запятые. */
        for (char c : a.query) {
            if (!std::isdigit(static_cast<unsigned char>(c)) &&
                c != ' ' && c != '*' && c != '/' && c != '-' && c != ',') {
                return "[запрещено] невалидное cron-выражение: " + a.query;
            }
        }
        /* Экранируем команду для shell. */
        std::string entry = a.query + " " + a.cli;
        std::string cmd = " (crontab -l 2>/dev/null; echo " +
                          shell::shell_quote(entry) + ") | crontab -";
        std::string out = shell::run_capture(cmd, 30);
        return out.empty() ? "[cron: добавлено]" : shell::cap(out, kDevopsMaxOutput);
    }, "Добавление cron-задачи");

    /* SSH. */
    reg.register_tool("ssh_exec", [](const ToolArgs& a) -> std::string {
        if (a.query.empty() || a.cli.empty())
            return "[ошибка] QUERY=host, CLI=команда";
        std::string host = security::sanitize_ident(a.query);
        /* Допускаем user@host формат. */
        if (host.empty()) {
            /* Попробуем без sanitize (для user@host:12345). */
            for (char c : a.query) {
                if (!std::isalnum(static_cast<unsigned char>(c)) &&
                    c != '@' && c != '.' && c != '-' && c != ':') {
                    return "[запрещено] невалидный хост: " + a.query;
                }
            }
            host = a.query;
        }
        std::string cmd = "ssh " + shell::shell_quote(host) + " " +
                          shell::shell_quote(a.cli);
        std::string out = shell::run_capture(cmd, 120);
        return out.empty() ? "[ssh: нет вывода]" : shell::cap(out, kDevopsMaxOutput);
    }, "Выполнение команды по SSH");
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
