#include "devops_tools.h"
#include "../../core/tools_registry.h"
#include "../../core/engine.h"

#include <cstdio>
#include <sstream>
#include <iostream>

namespace coder {
namespace devops {

namespace {

std::string run_capture(const std::string& cmd) {
    std::string full = cmd + " 2>&1";
    FILE* f = popen(full.c_str(), "r");
    if (!f) return "[ошибка] не удалось запустить: " + cmd;
    char buf[4096];
    std::string out;
    while (fgets(buf, sizeof(buf), f)) out += buf;
    pclose(f);
    if (out.size() > 10000) { out.resize(10000); out += "\n...[обрезано]"; }
    return out.empty() ? "[нет вывода]" : out;
}

} // anonymous namespace

void register_devops_tools() {
    auto& reg = ToolsRegistry::instance();

    /* Docker. */
    reg.register_tool("docker_build", [](const ToolArgs& a) -> std::string {
        std::string ctx = a.query.empty() ? "." : a.query;
        std::string file = a.path.empty() ? "" : " -f " + a.path;
        return run_capture("docker build" + file + " " + ctx);
    }, "Сборка Docker-образа");

    reg.register_tool("docker_run", [](const ToolArgs& a) -> std::string {
        return run_capture("docker run " + a.cli);
    }, "Запуск Docker-контейнера");

    reg.register_tool("docker_ps", [](const ToolArgs& a) -> std::string {
        std::string filter = a.query.empty() ? "" : " --filter " + a.query;
        return run_capture("docker ps" + filter + " --format 'table {{.Names}}\\t{{.Status}}\\t{{.Ports}}'");
    }, "Список Docker-контейнеров");

    reg.register_tool("docker_logs", [](const ToolArgs& a) -> std::string {
        if (a.query.empty()) return "[ошибка] укажи имя контейнера (QUERY)";
        return run_capture("docker logs --tail 100 " + a.query);
    }, "Логи Docker-контейнера");

    /* Systemd. */
    reg.register_tool("systemd_status", [](const ToolArgs& a) -> std::string {
        if (a.query.empty()) return "[ошибка] укажи имя сервиса (QUERY)";
        return run_capture("systemctl status " + a.query);
    }, "Статус systemd-сервиса");

    reg.register_tool("systemd_restart", [](const ToolArgs& a) -> std::string {
        if (a.query.empty()) return "[ошибка] укажи имя сервиса (QUERY)";
        return run_capture("sudo systemctl restart " + a.query);
    }, "Перезапуск systemd-сервиса");

    /* Nginx. */
    reg.register_tool("nginx_test", [](const ToolArgs& a) -> std::string {
        std::string cfg = a.query.empty() ? "" : " -c " + a.query;
        return run_capture("sudo nginx -t" + cfg);
    }, "Проверка конфигурации Nginx");

    reg.register_tool("nginx_reload", [](const ToolArgs&) -> std::string {
        return run_capture("sudo nginx -s reload");
    }, "Перезагрузка Nginx");

    /* Cron. */
    reg.register_tool("cron_list", [](const ToolArgs& a) -> std::string {
        std::string user = a.query.empty() ? "" : " -u " + a.query;
        return run_capture("crontab -l" + user);
    }, "Список cron-задач");

    reg.register_tool("cron_add", [](const ToolArgs& a) -> std::string {
        if (a.query.empty() || a.cli.empty())
            return "[ошибка] QUERY=cron_expression, CLI=команда";
        std::string entry = a.query + " " + a.cli;
        return run_capture("echo '" + entry + "' | crontab -");
    }, "Добавление cron-задачи");

    /* SSH. */
    reg.register_tool("ssh_exec", [](const ToolArgs& a) -> std::string {
        if (a.query.empty() || a.cli.empty())
            return "[ошибка] QUERY=host, CLI=команда";
        return run_capture("ssh " + a.query + " '" + a.cli + "'");
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
        {"devops_docker", "Docker best practices", kDockerSkill},
        {"devops_systemd", "systemd service management", kSystemdSkill},
        {"devops_nginx", "Nginx конфигурация", kNginxSkill}
    };
}

} // namespace devops
} // namespace coder
