#pragma once

/*
 * engine.h — Универсальный ReAct-движок AI-кодера.
 *
 * Рефакторинг agent.cpp: вынесена логика ReAct-цикла в универсальный компонент,
 * не привязанный к WordPress или любой другой доменной области.
 *
 * Сборка системного промпта: базовый + модульные промпты + навыки + режим.
 * Инструменты: через ToolsRegistry (динамическая регистрация модулями).
 */

#include "module_api.h"
#include "tools_registry.h"
#include "skills_manager.h"

#include <string>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <functional>

namespace coder {

/* Тип события агента для лога в UI. */
struct AgentEvent {
    enum Kind { Assistant, Tool, Status, Error } kind;
    std::string text;
};

/* Предложенная правка (план-режим). */
struct PendingWrite {
    std::string path;
    std::string content;
};

/* Общее состояние движка (защищается mtx). */
struct EngineState {
    mutable std::mutex mtx;
    std::condition_variable permission_cv;
    bool waiting_in_sync = false;

    /* Проект. */
    std::string project_dir;
    std::string php_bin;

    /* Удалённый доступ. */
    std::string wp_site_url;
    std::string wp_app_user;
    std::string wp_app_password;

    /* Деплой. */
    std::string deploy_proto;
    std::string deploy_host;
    std::string deploy_user;
    std::string deploy_pass;
    std::string deploy_port;
    std::string deploy_remote_dir;
    std::string wp_local_url;         // локальный сайт для проверки

    /* LLM / промпт. */
    std::string agent_system_prompt;  // пользовательский (пустой = kBaseSystemPrompt)
    std::string active_module;        // имя активного модуля

    /* Режимы. */
    int mode = 0;  // 0=Code, 1=Research, 2=Review
    bool plan_mode = false;

    /* Агент. */
    std::deque<AgentEvent> events;
    std::queue<std::string> inbox;
    std::condition_variable cv;
    std::thread worker;
    bool running = false;
    bool shutting_down = false;
    std::vector<PendingWrite> pending;
    std::string last_agent_task;

    /* Результат последнего ответа агента (для async mode). */
    std::string last_response;
    bool response_ready = false;
    std::condition_variable response_cv;

    /* Разрешения на доступ к файлам. */
    std::vector<std::string> allowed_external_paths;
    std::string pending_permission_path;
    bool waiting_for_permission = false;
    std::string once_path;
};

/* Callback-типы для взаимодействия с хостом (LLM, пути, настройки). */
struct HostCallbacks {
    /* LLM: отправить промпт, получить ответ. Возвращает true при успехе. */
    std::function<bool(const std::string& sys_prompt,
                       const std::string& user_prompt,
                       std::string& response)> llm_complete;

    /* Проверка подключения LLM. */
    std::function<bool()> llm_is_connected;

    /* Получение путей. */
    std::function<std::string()> path_data_dir;
    std::function<std::string()> path_config_dir;

    /* Настройки. */
    std::function<std::string(const std::string& key,
                              const std::string& def)> settings_get;
    std::function<void(const std::string& key,
                       const std::string& value)> settings_set;

    /* RAG. */
    std::function<bool(const std::string& path)> rag_process_document;
    std::function<std::string(const std::string& query, int k,
                              const std::string& path_filter)> rag_build_prompt;
    std::function<int()> rag_index_count;
};

class Engine {
public:
    static Engine& instance();

    /* Инициализация: передать callbacks хоста. */
    void init(HostCallbacks callbacks);

    /* Запуск worker-потока. */
    void start();

    /* Остановка worker-потока. */
    void stop();

    /* Постановка задачи в очередь. */
    void submit(const std::string& prompt);

    /* Ожидание готового ответа агента (timeout в мс). */
    std::string wait_response(int timeout_ms = 120000);

    /* Доступ к состоянию. */
    EngineState& state() { return state_; }
    const EngineState& state() const { return state_; }

    /* Доступ к callbacks хоста (для инструментов). */
    const HostCallbacks& callbacks() const { return cb_; }

    /* Управление событиями. */
    void push_event(AgentEvent::Kind k, const std::string& text);

    /* Доступ к UI-событиям. */
    const std::deque<AgentEvent>& events() const { return state_.events; }

    /* Разбор wp_action-блока. */
    struct Action {
        std::string tool, path, root, query, pattern, content, cli, url;
        int k = 6;
    };
    static std::string extract_action(const std::string& text, std::string& rest);
    static bool parse_action(const std::string& block, Action& a);

    /* Сборка полного системного промпта. */
    std::string build_system_prompt() const;

    /* Применение/отклонение предложенных правок. */
    void pending_apply(size_t idx);
    void pending_discard(size_t idx);

    /* Разрешения. */
    void permission_allow_once(const std::string& path);
    void permission_allow_always(const std::string& path);
    void permission_reject(const std::string& path);

private:
    EngineState state_;
    HostCallbacks cb_;

    void run_task(const std::string& task);
    void worker_main();

    /* Загрузка/сохранение настроек. */
    void load_settings();
    void save_settings();

    /* Проверка доступа к файлу за пределами проекта. */
    std::string check_external_permission(const std::string& abs_path);
};

/* Удобные глобальные accessor-ы. */
inline Engine& engine() { return Engine::instance(); }
inline EngineState& engine_state() { return Engine::instance().state(); }

} // namespace coder
