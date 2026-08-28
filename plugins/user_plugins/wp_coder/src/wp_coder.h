#pragma once

/*
 * wp_coder — AI-кодер для WordPress (MVP-каркас).
 *
 * Подключает только SDK хоста (include/plugins/plugin_api.h) и Dear ImGui.
 * Архитектура:
 *   - plugin_main : точка входа ll_plugin_*, меню, окна, отрисовка UI.
 *   - agent       : worker-поток с ReAct-циклом (planner ⇄ инструменты).
 *   - tools       : read/write_file, grep_hooks, php_lint, rag_index/query.
 *   - project     : локальный WP (путь + php-cli), настройки.
 *
 * Потокобезопасность: хост-API (llm_complete_ex/rag_*) дёргаются из worker-потока;
 * общий доступ к состоянию защищён std::mutex. UI (ll_plugin_render) только читает.
 */

#include "plugins/plugin_api.h"
#include <string>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>

/* Глобальные хендлы хоста (определены в plugin_main.cpp). */
extern LlamaPluginHost* g_host;
extern const LlamaHostApi* g_api;

/* Тип события агента для лога в UI. */
struct AgentEvent {
    enum Kind { Assistant, Tool, Status, Error } kind;
    std::string text;
};

/* Предложенная правка (план-режим): ждёт подтверждения пользователя. */
struct PendingWrite {
    std::string path;     // относительный путь
    std::string content;  // новое содержимое
};

/* Навык (skill): инструкция, подключаемая к промпту планировщика (как в opencode). */
struct Skill {
    std::string name;
    std::string description;
    std::string body;     // текст-инструкция
};

/* Общее состояние плагина (защищается state.mtx). */
struct WpCoderState {
    std::mutex mtx;
    std::string project_dir;          // локальный корень WP (абс. путь)
    std::string php_bin;              // php-cli (напр. "php" в PATH)
    /* Удалённый WP (REST, app_password). */
    std::string wp_site_url;          // https://example.com (без слэша в конце)
    std::string wp_app_user;          // логин приложения
    std::string wp_app_password;      // application password (без пробелов)
    /* Деплой (rsync / ftp / sftp). */
    std::string deploy_proto;         // rsync | ftp | ftps | sftp
    std::string deploy_host;
    std::string deploy_user;
    std::string deploy_pass;
    std::string deploy_port;
    std::string deploy_remote_dir;    // полный путь на сервере
    std::string wp_local_url;         // локальный сайт для проверки (http://localhost:8080)
    /* Навыки (skills) и ролевой режим. */
    std::vector<Skill> skills;                // загруженные навыки
    std::vector<std::string> active_skills;  // имена включённых в промпт
    int mode = 0;                             // 0=Code, 1=Research, 2=Review
    std::deque<AgentEvent> events;    // лента событий агента (для UI)
    std::queue<std::string> inbox;    // входящие промпты пользователя
    std::condition_variable cv;       // сигнал worker-у о новом промпте
    std::thread worker;
    bool running = false;             // агент сейчас в цикле
    bool shutting_down = false;
    bool plan_mode = false;           // true = правки только предлагаются, не применяются
    std::vector<PendingWrite> pending; // предложенные правки (план-режим)
};

/* Применение/отклонение предложенной правки (план-режим). */
void pending_apply(size_t idx);
void pending_discard(size_t idx);

/* Загрузка навыков из <data_dir>/wp_coder/skills и каталога плагина. */
void skills_load();

extern WpCoderState g_state;

/* --- project.cpp --- */
void project_load_settings();
void project_save_settings();
void project_detect_php();
std::string project_resolve(const std::string& rel); // относительный -> абсолютный

/* --- tools.cpp --- */
/* Выполняет один вызов инструмента. Возвращает текст-результат (для модели). */
std::string tool_run(const std::string& tool,
                     const std::string& arg_path,
                     const std::string& arg_root,
                     const std::string& arg_query,
                     const std::string& arg_pattern,
                     int arg_k,
                     const std::string& arg_content,
                     const std::string& arg_cli,
                     const std::string& arg_url);

/* --- agent.cpp --- */
void agent_start();      // запуск worker-потока (из ll_plugin_init)
void agent_stop();       // остановка (из ll_plugin_shutdown)
void agent_submit(const std::string& prompt); // постановка задачи в очередь

/* Вспомки настроек (храним как JSON-строку: "\"value\""). */
std::string setting_get_str(const std::string& key, const std::string& def);
void setting_set_str(const std::string& key, const std::string& value);
