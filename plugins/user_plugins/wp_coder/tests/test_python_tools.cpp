/*
 * test_python_tools.cpp — безопасность и валидация Python-инструментов (4.4).
 *
 * Тестируем ветки валидации ДО запуска команд (pip_install с пустым именем).
 * Функции, выполняющие команды (python_run, pytest_run и т.д.), не вызываются —
 * они тестируются только интеграционно, вручную.
 */

#include "test_framework.h"
#include "../core/tools_registry.h"
#include "../core/engine.h"
#include "../modules/python/python_tools.h"

using namespace coder;

static void init_engine_and_python() {
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ChatMsg>&, LlmReply&) { return false; };
    cb.llm_complete = [](const std::string&, const std::string&, std::string&) { return false; };
    cb.llm_is_connected = []() { return false; };
    cb.chat_event = [](const std::string&) {};
    Engine::instance().init(cb);
    python::register_python_tools();
}

TEST(pip_install_empty_rejected) {
    init_engine_and_python();
    ToolArgs a;  // query пуст
    std::string r = ToolsRegistry::instance().run("pip_install", a);
    ASSERT_TRUE(r.find("укажи имя пакета") != std::string::npos);
}

TEST(python_tools_registered) {
    init_engine_and_python();
    auto& reg = ToolsRegistry::instance();
    ASSERT_TRUE(reg.has("python_run"));
    ASSERT_TRUE(reg.has("pip_install"));
    ASSERT_TRUE(reg.has("pytest_run"));
    ASSERT_TRUE(reg.has("venv_create"));
    ASSERT_TRUE(reg.has("python_lint"));
    ASSERT_TRUE(reg.has("django_manage"));
}