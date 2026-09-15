#pragma once

/*
 * engine.h — Универсальный ReAct-движок AI-кодера.
 *
 * Цикл агента построен на многоходовой истории диалога (session):
 * каждый шаг отправляет model полную последовательность сообщений
 * (системный промпт + user задача + ассистентские ответы + RESULT).
 * Это включает префикс-кэширование промпта у провайдера/локального сервера,
 * уменьшает число лишних шагов и даёт честные метрики из usage.
 */

#include "module_api.h"
#include "tools_registry.h"
#include "skills_manager.h"
#include "tool_protocol.h"

#include <string>
#include <vector>
#include <deque>
#include <atomic>
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

/* Одно сообщение многоходовой истории агента. */
struct ChatMsg {
    std::string role;    // "user" | "assistant"
    std::string content;
};

/* Состояние агента (FSM, 2.3). Переходы:
 *   Idle → Planning → Executing ⇄ WaitingPermission → Done | Aborted
 * Вместо разрозненных флагов running/waiting_for_permission. */
enum class AgentState {
    Idle,               // задачи нет, worker ждёт
    Planning,           // Planner: первый LLM-вызов — план действий
    Executing,          // AgentLoop: ReAct-цикл (вызовы инструментов)
    WaitingPermission,  // ожидание решения пользователя по доступу
    Done,               // задача завершена успешно
    Aborted             // прервано пользователем или фатальная ошибка
};

/* Человекочитаемое имя состояния (для статуса в UI). */
inline const char* agent_state_name(AgentState s) {
    switch (s) {
        case AgentState::Idle:              return "Idle";
        case AgentState::Planning:          return "План";
        case AgentState::Executing:         return "Выполнение";
        case AgentState::WaitingPermission: return "Ожидание разрешения";
        case AgentState::Done:              return "Готово";
        case AgentState::Aborted:           return "Прервано";
    }
    return "?";
}

/* Структурированный ответ LLM (заполняется через HostCallbacks::llm_chat). */
struct LlmReply {
    bool ok = false;
    std::string content;
    std::string finish_reason = "stop";
    int prompt_tokens = 0;
    int completion_tokens = 0;
    std::string error;
};

/* Общее состояние движка (защищается mtx, кроме атомарных флагов). */
struct EngineState {
    mutable std::mutex mtx;
    std::condition_variable permission_cv;
    bool waiting_in_sync = false;

    /* Метрики последнего ответа (честные, из usage LLM-вызовов). */
    double last_response_time = 0;          // полное время задачи, сек
    double llm_total_time = 0;              // суммарное время LLM-вызовов, сек
    int last_tokens_generated = 0;          // completion_tokens
    int total_prompt_tokens = 0;
    int total_completion_tokens = 0;
    double last_tokens_per_second = 0;
    int steps = 0;                          // число шагов ReAct

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
    mutable std::string cached_system_prompt;  // кэш собранного промпта
    mutable bool prompt_dirty = true;          // флаг необходимости пересборки

    /* Режимы. */
    int mode = 0;  // 0=Code, 1=Research, 2=Review
    bool plan_mode = false;
    /* B1: перед исполнением модель составляет краткий план действий (без вызова
     * инструментов), который затем добавляется в сессию как контекст. */
    bool use_planning = true;

    /* Продолжать сессию при новом сообщении (по умолчанию — да).
     * Если false — каждый submit() очищает сессию и начинает заново.
     * Если true — новое сообщение добавляется к существующей сессии,
     * план и контекст сохраняются между запросами. */
    bool continue_conversation = true;

    /* Таймаут одного LLM-вызова, мс (по умолчанию 120 с).
     * При превышении агент прерывает ожидание и сообщает об ошибке,
     * не дожидаясь ответа провайдера (2.2). */
    int llm_timeout_ms = 120000;

    /* Агент. */
    std::deque<AgentEvent> events;
    std::queue<std::string> inbox;
    std::condition_variable cv;
    std::thread worker;
    /* Текущее состояние FSM (заменяет флаги running/waiting_for_permission). */
    AgentState state = AgentState::Idle;
    /* Lifecycle-флаг движка (не состояние агента): stop() запрошен —
     * worker-поток должен выйти из цикла. */
    bool shutting_down = false;
    std::atomic<bool> abort_requested{false};
    std::vector<PendingWrite> pending;
    std::string last_agent_task;

    /* Многоходовая сессия текущей задачи. */
    std::vector<ChatMsg> session;
    /* Флаг: следующий входящий запрос продолжает текущую сессию (после
     * паузы на разрешение), а не начинает новую. */
    bool preserve_session = false;

    /* Результат последнего ответа агента (для async mode). */
    std::string last_response;
    bool response_ready = false;
    std::condition_variable response_cv;

    /* Разрешения на доступ к файлам. */
    std::vector<std::string> allowed_external_paths;
    std::string pending_permission_path;
    std::string once_path;

    /* A1: последние вызовы инструментов (fingerprint) для детекта зацикливания. */
    std::deque<std::string> recent_calls;

    /* B2: кэш repo_map на текущую задачу — не перечитываем структуру проекта
     * на каждом шаге, если корень не менялся. Сбрасывается в начале run_task. */
    std::string repo_map_cache_root;
    std::string repo_map_cache_result;
};

/* Проверка, находится ли путь за пределами project_dir. */
inline bool is_path_outside(const std::string& abs_path, const std::string& project_dir) {
    if (project_dir.empty()) return false;
    auto normalize = [](const std::string& s) -> std::string {
        std::string r;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') continue;
            if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '.') {
                if (i + 2 >= s.size() || s[i + 2] == '/') { i += 1; continue; }
            }
            r += s[i];
        }
        return r;
    };
    std::string norm_path = normalize(abs_path);
    std::string norm_root = normalize(project_dir);
    if (!norm_root.empty() && norm_root.back() != '/') norm_root += '/';
    if (norm_path.find(norm_root) == 0) return false;
    if (norm_path == normalize(project_dir)) return false;
    return true;
}

/* Проверка, разрешён ли путь через список allowed. */
inline bool is_path_allowed(const std::string& abs_path,
                            const std::string& project_dir,
                            const std::vector<std::string>& allowed) {
    if (!is_path_outside(abs_path, project_dir)) return true;
    for (const auto& pattern : allowed) {
        if (!pattern.empty() && pattern.back() == '*') {
            if (abs_path.find(pattern.substr(0, pattern.size() - 1)) == 0) return true;
        } else {
            if (abs_path == pattern) return true;
        }
    }
    return false;
}

/* Callback-типы для взаимодействия с хостом (LLM, пути, настройки). */
struct HostCallbacks {
    /* LLM: многоходовой запрос. messages — история диалога без system. */
    std::function<bool(const std::string& sys_prompt,
                       const std::vector<ChatMsg>& messages,
                       LlmReply& out)> llm_chat;

    /* LLM: одногилый запрос (legacy fallback, если хост не поддерживает
     * llm_chat). Возвращает true при успехе, resp — текст ответа. */
    std::function<bool(const std::string& sys_prompt,
                       const std::string& user_prompt,
                       std::string& resp)> llm_complete;

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

    /* Прогресс в чат приложения (через agent_mode_push_event). */
    std::function<void(const std::string& event)> chat_event;
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

    /* Постановка задачи в очередь. Если это повтор задачи после паузы на
     * разрешение (preserve_session), продолжает текущую сессию диалога. */
    void submit(const std::string& prompt);

    /* Ожидание готового ответа агента (timeout в мс). */
    std::string wait_response(int timeout_ms = 120000);

    /* Прерывание текущей задачи (проверяется между шагами цикла). */
    void request_abort();

    /* Очистить сессию и план — начать с чистого листа.
     * Вызывается из UI или при смене задачи пользователем. */
    void clear_session();

    /* Доступ к состоянию. */
    EngineState& state() { return state_; }
    const EngineState& state() const { return state_; }

    /* Доступ к callbacks хоста (для инструментов). */
    const HostCallbacks& callbacks() const { return cb_; }

    /* Управление событиями. */
    void push_event(AgentEvent::Kind k, const std::string& text);

    /* Доступ к UI-событиям. */
    const std::deque<AgentEvent>& events() const { return state_.events; }

    /* Разбор блока вызова инструмента — вынесен в tool_protocol.h (Фаза D2).
     * Здесь остаются алиасы для обратной совместимости с существующим кодом. */
    using Action = coder::Action;
    static std::string extract_action(const std::string& text, std::string& rest) {
        return coder::extract_action(text, rest);
    }
    static bool parse_action(const std::string& block, Action& a) {
        return coder::parse_action(block, a);
    }

    /* Сборка полного системного промпта. */
    std::string build_system_prompt() const;

    /* Инвалидация кэша промпта (вызывать при изменении настроек). */
    void invalidate_prompt_cache() const { state_.prompt_dirty = true; }

    /* Применение/отклонение предложенных правок. */
    void pending_apply(size_t idx);
    void pending_discard(size_t idx);

    /* Разрешения. */
    void permission_allow_once(const std::string& path);
    void permission_allow_always(const std::string& path);
    void permission_reject(const std::string& path);

    /* Проверка доступа к файлу за пределами проекта. Возвращает пустую строку
     * при разрешении, иначе текст отказа (и переводит агента в ожидание). */
    std::string check_external_permission(const std::string& abs_path);

    /* Настройки. */
    void load_settings();
    void save_settings();

    /* Сжатие слишком длинной сессии: старые RESULT-сообщения заменяются
     * кратким заголовком. Выполняется внутри SessionStore::trim(). */
    void trim_session_test();

    /* Тестовый доступор: текущая сессия диалога (для юнит-тестов). */
    const std::vector<ChatMsg>& session_for_test() const { return state_.session; }

    /* FSM (2.3): переход состояния. Публикует observer-событие
     * AgentEvent::Status «state: <имя>» — видно в окне AI Coder. */
    void set_state(AgentState s);

private:
    EngineState state_;
    HostCallbacks cb_;

    void run_task(const std::string& task);
    void worker_main();
};

/* Удобные глобальные accessor-ы. */
inline Engine& engine() { return Engine::instance(); }
inline EngineState& engine_state() { return Engine::instance().state(); }

} // namespace coder