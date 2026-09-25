/*
 * test_python_tools.cpp — безопасность и валидация Python-инструментов (4.4).
 *
 * Тестируем ветки валидации ДО запуска команд (pip_install с пустым именем).
 * Функции, выполняющие команды (python_run, pytest_run и т.д.), не вызываются —
 * они тестируются только интеграционно, вручную.
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/json.h"
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
    test_support::approve_all_permissions();
    python::register_python_tools();
}

TEST(pip_install_empty_rejected) {
    init_engine_and_python();
    /* И1.6: query обязателен по схеме — пустой вызов отсекается валидацией. */
    ToolArgs a;  // query пуст
    std::string r = ToolsRegistry::instance().run("pip_install", a);
    ASSERT_TRUE(r.find("invalid arguments") != std::string::npos);
    ASSERT_TRUE(r.find("query") != std::string::npos);
    /* Явно переданный пустой query доходит до обработчика. */
    json::JsonValue args = json::JsonValue::object();
    args.set("query", "");
    std::string r2 = ToolsRegistry::instance().run("pip_install", args);
    ASSERT_TRUE(r2.find("укажи имя пакета") != std::string::npos);
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