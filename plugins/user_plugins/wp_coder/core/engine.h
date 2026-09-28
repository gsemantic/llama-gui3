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

#include "abort.h"
#include "compaction.h"
#include "module_api.h"
#include "tools_registry.h"
#include "skills_manager.h"
#include "tool_protocol.h"
#include "permission_engine.h"
#include "message.h"

#include <string>
#include <vector>
#include <deque>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <queue>
#include <thread>
#include <functional>

namespace coder {

/* Тип события агента для лога в UI.
 *
 * И6.8: отмена и сбой — РАЗНЫЕ виды, а не один «Error». «Стоп» — это
 * намерение пользователя, и UI обязан показать «прервано», а не «ошибка»:
 * иначе человек, нажавший кнопку, читает о том, что у него сломалось. */
struct AgentEvent {
    enum Kind { Assistant, Tool, Status, Error, Aborted } kind;
    std::string text;
};

/* Предложенная правка (план-режим). */
struct PendingWrite {
    std::string path;
    std::string content;
};

/* И4.6: пункт плана задачи (порт opencode tool/todo.ts).
 *
 * Список нужен не для красоты, а по двум причинам. Первая: у агента нет
 * памяти между шагами, кроме истории, и длинный план, выкованный в
 * первом сообщении, к шестому шагу уже не влияет на решение. Вторая:
 * пользователь не видит, что агент собирается делать, и может вмешаться
 * после третьего шага, а не после двенадцатого.
 *
 * Статусы и приоритеты — строковые константы, а не enum: значения приходят
 * от модели текстом, и нераспознанное значение должно быть видно в
 * интерфейсе как есть, а не молча превращаться в дефолт. */
struct TodoItem {
    std::string id;
    std::string content;
    /* pending | in_progress | completed | cancelled */
    std::string status = "pending";
    /* low | medium | high */
    std::string priority = "medium";
};

inline const char* kTodoStatuses[] = {"pending", "in_progress", "completed",
                                     "cancelled"};
inline const char* kTodoPriorities[] = {"low", "medium", "high"};

/* Состояние агента (FSM, 2.3). Переходы:
 *   Idle → Planning → Executing ⇄ WaitingPermission → Done | Aborted | Error
 * Вместо разрозненных флагов running/waiting_for_permission. */
enum class AgentState {
    Idle,               // задачи нет, worker ждёт
    Planning,           // Planner: первый LLM-вызов — план действий
    Executing,          // AgentLoop: ReAct-цикл (вызовы инструментов)
    WaitingPermission,  // ожидание решения пользователя по доступу
    Done,               // задача завершена успешно
    /* И6.8: отмена пользователем и сбой разведены. Раньше «фатальная ошибка»
     * тоже попадала в Aborted, а задача, провалился без единого ответа,
     * вообще объявлялась Done — то есть неудача выглядела как успех. */
    Aborted,            // прервано пользователем
    Error               // сбой: провайдер недоступен, ход пуст, инструмент упал
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
        case AgentState::Error:             return "Ошибка";
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
/* И6.8: чем закончилась задача. */
enum class TaskOutcome {
    None = 0,   /* задача ещё идёт */
    Completed,  /* ход завершён, ответ получен */
    Aborted,    /* пользователь нажал «стоп» */
    Failed      /* сбой: провайдер, пустой ход, упавший инструмент */
};

/* И6.8: названия исходов различаются, потому что по ним UI рисует строку в
 * списке последних задач. Совпавшие строки означали бы, что «прервано» и
 * «ошибка» выглядят одинаково, — то есть различение было бы сделано, но не
 * показано. */
inline const char* task_outcome_name(TaskOutcome outcome) {
    switch (outcome) {
        case TaskOutcome::None:      return "идёт";
        case TaskOutcome::Completed: return "выполнено";
        case TaskOutcome::Aborted:   return "прервано";
        case TaskOutcome::Failed:    return "ошибка";
    }
    return "неизвестно";
}

/*
 * И6.8: исход → терминальное состояние.
 *
 * Отдельная функция, а не переключатель внутри cleanup() в run_task: там он
 * был на три строчки глубже лямбды, и единственный способ его проверить
 * был — прогнать весь синглтон Engine целиком, что тест делал в первый раз:
 * он зависал на остаточной очереди предыдущего теста. Правило вынесено,
 * потому что его и надо проверять: «сбой не выглядит как успех».
 */
inline AgentState terminal_state_for(TaskOutcome outcome) {
    switch (outcome) {
        case TaskOutcome::Aborted: return AgentState::Aborted;
        case TaskOutcome::Failed:  return AgentState::Error;
        case TaskOutcome::Completed:
        case TaskOutcome::None:    return AgentState::Done;
    }
    return AgentState::Done;
}

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

    /* Настройки агента (5.3) — переопределяют дефолты из core/limits.h. */
    int max_steps = 12;            // лимит шагов ReAct на задачу (kMaxSteps)
    size_t session_budget = 60000; // бюджет символов истории сессии (kSessionBudget)

    /* --- И7.2: учёт контекста ---
     *
     * measured_input_tokens — input-токены ПОСЛЕДНЕГО запроса, какие их
     * прислал провайдер. Не сумма за сессию: сумма росла бы монотонно и
     * рано или поздно объявила бы переполнение в пустой истории. Ноль —
     * «провайдер usage не прислал» (первый запрос, старый хост), и это
     * не то же самое, что «контекст пуст»: тогда считается оценка.
     *
     * Лимиты и настройки — из настроек плагина (load_settings), потому что
     * у хоста их взять нечем: в ABI нет ни окна модели, ни лимита ответа.
     * Нулевое значение везде означает «не задано» — и тогда переполнение
     * не объявляется вовсе (compaction::is_overflow), то есть агент
     * работает как раньше, без тихой порчи истории. */
    long long measured_input_tokens = 0;
    compaction::ModelLimits model_limits;
    compaction::CompactionConfig compaction_config;

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
    /* И6.7: токен ОТМЕННОГО хода. Отдельный от abort_requested потому, что
     * у отмены есть владелец и время жизни: ход начался — ход кончился.
     * Общий флаг жил бы вечно, и отмену нельзя было бы снять, а второе
     * нажатие «стоп» отменило бы СЛЕДУЮЩУЮ задачу. nullptr — ход без
     * токена (тесты, служебные вызовы): отменять нечем, и это не ошибка. */
    std::shared_ptr<AbortToken> turn_abort;
    /* И6.8: чем закончилась задача. Одно поле вместо вывода «состояние !=
     * Executing»: по состоянию нельзя отличить успех от сбоя, а именно
     * это и нужно UI, и именно это терялось. */
    TaskOutcome outcome = TaskOutcome::None;
    /* Почему — одной строкой для показа пользователю и для лога. */
    std::string outcome_reason;
    std::vector<PendingWrite> pending;
    std::string last_agent_task;

    /* Многоходовая сессия текущей задачи — И5.7: структура, а не список
     * строк. Один ход агента = одно сообщение с частями внутри, поэтому
     * «последний ответ модели» больше не ищется эвристикой по префиксу
     * «RESULT [x]:», а адресуется частью. Для модели история собирается
     * в реплики функцией to_model_messages (core/message.h). */
    std::vector<Message> session;
    /* Идентификатор сохраняемой сессии (И5.6). Пусто — сессия ещё ни разу
     * не записывалась; под локом не меняется иначе, чем сбросом. */
    std::string session_id;
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

    /* И4.6: план задачи. Сбрасывается при новой задаче (run_task) — список
     * прошлой задачи в промпте новой только сбивает. */
    std::vector<TodoItem> todos;

    /* B2: кэш repo_map на текущую задачу — не перечитываем структуру проекта
     * на каждом шаге, если корень не менялся. Сбрасывается в начале run_task. */
    std::string repo_map_cache_root;
    std::string repo_map_cache_result;
};

/* Обнуление метрик задачи (И7.2).
 *
 * Отдельная функция, а не строки внутри run_task: run_task достижим
 * только через worker-поток синглтона, и тест на нём зависал бы на
 * общей очереди предыдущего теста — то есть проверял бы не своё (та же
 * причина, по которой в И6.8 правило «исход → состояние» вынесено из
 * лямбды cleanup()).
 *
 * Лок НЕ берётся: вызывающий уже держит state_.mtx, а он нерекурсивный
 * и общий с UI-потоком. */
void reset_task_metrics(EngineState& state);

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
    /* LLM: многоходовой запрос. messages — история диалога без system,
     * собранная из сессии (to_model_messages). */
    std::function<bool(const std::string& sys_prompt,
                       const std::vector<ModelMessage>& messages,
                       LlmReply& out)> llm_chat;

    /* LLM: одногилый запрос (legacy fallback, если хост не поддерживает
     * llm_chat). Возвращает true при успехе, resp — текст ответа. */
    std::function<bool(const std::string& sys_prompt,
                       const std::string& user_prompt,
                       std::string& resp)> llm_complete;

    /*
     * LLM: потоковый запрос (И6.4). Слот заполняется, только если у хоста
     * есть поле llm_chat_stream (проба в ll_plugin_init), иначе остаётся
     * пустым — и это НЕ ошибка: хост старый, и вызывающий обязан уйти на
     * блокирующий llm_chat. Типы здесь только std::function: ядро плагина
     * от ABI хоста не зависит (D-7), мост живёт в src/llm_stream_shim.cpp.
     *
     * Вызов возвращает false, если хост не умеет стриминг ИЛИ поток не
     * запустился; тогда on_done НЕ зовётся. Иначе on_done придёт ровно
     * один раз, и до него колбэки ещё будут приходить.
     *
     * kind в on_delta: 0 = текст, 1 = «размышление» (LLAMA_STREAM_DELTA_*).
     */
    std::function<bool(const std::string& sys_prompt,
                       const std::vector<ModelMessage>& messages,
                       const std::string& request_json,
                       /* handle этого потока (И6.6). Сообщается ДО входа в
                        * хоста, потому что отменять можно только живой
                        * поток. handle перестаёт быть действительным после
                        * on_done, и отмена старого handle обязана быть
                        * безвредной, а не аварийной. */
                       std::function<void(void* handle)> on_started,
                       std::function<void(const char* text, int kind)> on_delta,
                       std::function<void(const char* call_id,
                                          const char* tool_name,
                                          const char* fragment)> on_tool_delta,
                       std::function<void(const std::string& result_json)> on_done)> llm_chat_stream;

    /*
     * И6.6: прервать ЖИВОЙ поток. handle — тот же, что передан в
     * llm_chat_stream; второго идентификатора быть не может, иначе отмена
     * не найдёт свой поток и тихо ничего не сделает.
     *
     * Слот пуст, если хост старый (нет поля в ABI) — тогда отмена означает
     * «перестать ждать», а не «остановить генерацию», и это различие
     * названо прямо в llm_client.h.
     */
    std::function<void(void* handle)> llm_chat_cancel;

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

    /* Resume сессии (5.2): сохранение/загрузка диалога на диск (И5.6).
     * Файл: <data_dir>/wp_coder/sessions/<session_id>.json.
     *
     * Формат и атомарная запись — в core/session_store.h; здесь только
     * перевод между историей движка и файлом. */
    void save_session();
    void load_session();

    /* Доступ к состоянию. */
    EngineState& state() { return state_; }
    const EngineState& state() const { return state_; }

    /* Доступ к callbacks хоста (для инструментов). Неконстантная
     * перегрузка нужна ToolContext: PermissionGate пишет в настройки
     * (settings_set) при выдаче постоянного разрешения. */
    HostCallbacks& callbacks() { return cb_; }
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

    /* Ответ на запрос системы разрешений И2 из UI: id приходит из
     * PermissionEngine::pending(). once/always/reject. */
    void permission_reply(uint64_t id, PermissionReply how);
    /* Снимок ожидающих запросов для панели (копия, лок отпускается). */
    std::vector<PermissionRequest> permission_pending() const;

    /* Проверка доступа к файлу за пределами проекта. Возвращает пустую строку
     * при разрешении, иначе текст отказа (и переводит агента в ожидание). */
    std::string check_external_permission(const std::string& abs_path);

    /* Сжатие истории, если она превысила бюджет.
     *
     * Единственная безопасная точка входа: state_.mtx берётся ровно один
     * раз внутри. Вызывать из кода, который уже держит лок, нельзя —
     * мьютекс нерекурсивный (engine.h), повторный захват вешает GUI
     * (D1). Правило сжатия — в compress_history (core/message.h): там
     * текст, а здесь только лок и бюджет. */
    void trim_history_if_needed();

    /* Настройки. */
    void load_settings();
    void save_settings();

    /* Тестовый доступор: текущая сессия диалога (для юнит-тестов). */
    const std::vector<Message>& session_for_test() const {
        return state_.session;
    }

    /* Тестовый вызов сжатия: тот же trim_history_if_needed, но без
     * проверки «нужно ли» — тест проверяет правило, а не условие входа. */
    void trim_session_test();

    /* FSM (2.3): переход состояния. Публикует observer-событие
     * AgentEvent::Status «state: <имя>» — видно в окне AI Coder. */
    void set_state(AgentState s);

    /* Система разрешений (И2). Правила живут в сессии: ответ «всегда»
     * должен переживать отдельные вызовы инструментов, поэтому движок —
     * член Engine, а не локальная переменная цикла. */
    PermissionEngine& permissions() { return permissions_; }
    const PermissionEngine& permissions() const { return permissions_; }

private:
    EngineState state_;
    HostCallbacks cb_;
    PermissionEngine permissions_;

    void run_task(std::string task);
    void worker_main();

    /* Путь к файлу сохранённой сессии (resume, 5.2): <data_dir>/wp_coder/session.json. */
    std::string session_file_path() const;
};

/* Удобные глобальные accessor-ы. */
inline Engine& engine() { return Engine::instance(); }
inline EngineState& engine_state() { return Engine::instance().state(); }

} // namespace coder