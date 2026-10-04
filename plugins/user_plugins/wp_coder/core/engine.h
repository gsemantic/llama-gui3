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
#include "agent_registry.h"
#include "compaction.h"
#include "harness_profile.h"
#include "instruction.h"
#include "module_api.h"
#include "tools_registry.h"
#include "skills_manager.h"
#include "tool_protocol.h"
#include "permission_engine.h"
#include "message.h"
#include "limits.h"
#include "snapshot.h"

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

/* --- И8.7: кто выполняет текущий ход ---
 *
 * Один и тот же движок исполняет и ход сессии, и вложенный ход
 * субагента (инструмент `task`). Различать их нужно ровно в двух
 * местах, и оба читают ОДНО это место:
 *   - сборка системного промпта: у субагента своя роль и свой набор
 *     видимых инструментов (agent::Info);
 *   - enforcement в ToolRunner::run: правила агента накладываются на
 *     правила сессии сверху (И8.4), и у субагента своя история
 *     зацикливания.
 *
 * Почему не «второй Engine» и не параметр в ToolRunner: состояние
 * движка — проект, режим, план, разрешения, метрики, отмена — нужно
 * ребёнку целиком, а второй экземпляр означал бы, что половина
 * состояния живёт в двух местах. И передавать «кто я» параметром
 * пришлось бы через ToolContext в каждый инструмент, тогда как
 * спрашивать надо в двух точках.
 *
 * Живёт в EngineState, а не в Engine: область меняется на время
 * вложенного хода и обязана вернуться (ScopedAgentScope), а
 * состояние — то, что копируется под локом вместе со всем прочим.
 */
struct RunScope {
    /* Имя выбранного агента; пусто — ход сессии без агента. */
    std::string agent;
    /* Правила и промпт агента. nullptr — субагента нет, решения
     * принимают правила сессии (PermissionEngine).
     *
     * Указатель НЕ const, и это единственное место, где «замороженность»
     * правил агента (И8.4) размывается: расти может approve() — ответ
     * пользователя «всегда», который обязан дойти и до правик ребёнка,
     * иначе кнопка «всегда» работала бы в сессии и не работала у
     * субагента. Ничего другого mutator'а у Info нет, а rules() отдаёт
     * const&, поэтому изменить замороженный набор нельзя. */
    std::shared_ptr<agent::Info> info;
    /* Глубина вложенности: 0 — родитель. Считается здесь, потому что
     * вопрос «не слишком ли глубоко» (И8.8) — это вопрос «кто сейчас
     * выполняет ход», а не свойство инструмента. */
    int depth = 0;
    /* История зацикливания СВОЕГО хода; nullptr — использовать
     * state_.recent_calls (родительскую). Своя по двум причинам:
     * чужой счётчик видел бы вызовы ребёнка как вызовы родителя, и
     * три одинаковых чтения субагентом спросили бы у пользователя
     * «зацикливание?» вместо родителя. */
    std::shared_ptr<std::deque<std::string>> loop_calls;
};

/* Задание ребёнку: кто он, что сделать, по каким правилам и с какой
 * историей. Заполняется и для обычного вызова `task`, и для фоновой
 * задачи (И8.14) — а выполняется ОДНОЙ функцией (run_child_turn,
 * core/subagent.cpp), потому что «сделать задачу ребёнком» в двух местах
 * разошлось бы первым же изменением.
 *
 * Правила и глубина копируются В МОМЕНТ ПОСТАНОВКИ, а не в момент
 * выполнения, и это не оптимизация, а безопасность: к моменту выполнения
 * фоновой задачи область выполнения принадлежит СЕССИИ, и ребёнок,
 * поставленный СУБАГЕНТОМ, выполнился бы по правилам сессии — то есть его
 * внук получил бы права не того, кто звал. «Кто вложил задачу» — это и
 * есть тот, чьи правила наследуются (И8.10). */
struct SubagentJob {
    std::string agent;        /* имя субагента */
    std::string description;  /* короткое имя задачи (видит пользователь) */
    std::string prompt;       /* задание для субагента */
    std::string session_id;   /* идентификатор задачи, выдан при постановке */
    std::string title;        /* название дочерней сессии */
    std::string parent_id;    /* диалог, из которого задача поставлена */
    std::shared_ptr<agent::Info> info;  /* правила ребёнка на момент постановки */
    int depth = 0;            /* глубина вложенности, зафиксированная при постановке */
    int max_steps = 0;        /* предел шагов ребёнка */
    std::vector<Message> seed;/* история продолжения (task_id, И8.9) */
    /* Продолжение существующей задачи, а не новая: нужно только для
     * события «задача … создана/продолжена», но по событию человек и
     * понимает, что произошло. */
    bool resumed = false;
};

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
    /* И9.1: инструкции проекта и пользователя, готовые к вставке.
     *
     * Читаются ОДИН раз (см. Engine::ensure_instructions) и живут в
     * состоянии, а не перечитываются на каждой сборке промпта: сборка
     * идёт и на каждом todowrite, а источником может быть URL, и повторная
     * загрузка означала бы сетевой запрос посреди хода.
     *
     * mutable — по той же причине, что у cached_system_prompt и
     * prompt_dirty: сборка промпта константна (её зовёт и вложенный ход
     * субагента через ссылку на движок), а наполнить состояние должен
     * тот же вызов, который его читает. Приватность состояния важнее
     * константности функции, иначе ленивая загрузка потребовала бы
     * неконстантного пути вызова на каждом месте. */
    mutable std::vector<Instruction> instructions;
    mutable bool instructions_loaded = false;
    /* И9.1: значение настройки `wp_coder.instructions` (JSON-массив
     * строк), прочитанное в load_settings. Разбирается в список источников
     * лениво — при первой сборке промпта, где уже известен корень
     * проекта. */
    std::string instructions_config;
    /* И9.7: профиль harness. `wp_coder.profile` — имя (без .json),
     * `wp_coder.profiles_dir` — каталог, а profiles_bundled_dir приходит
     * из src/plugin_main.cpp тем же путём, что и каталог навыков
     * (WP_CODER_SKILLS_DIR): ядро не знает, где себя лежит, и знать не
     * должно (D-7).
     *
     * Загрузка ПРЯМАЯ и в load_settings, а не ленивая, как инструкции
     * (И9.1): файл локальный, без сети, а профиль сужает права, и
     * право, которое применяется на первом же ходе, должно быть
     * применено ДО первого хода, а не одновременно с ним. */
    std::string profile_name;
    std::string profiles_dir;
    std::string profiles_bundled_dir;
    bool has_profile = false;
    harness::Profile profile;
    /* Почему профиль не применился (пусто — применился или не задан).
     * Показывается в логе и в панели: молча неприменённый профиль
     * выглядел бы как «настройка не работает». */
    std::string profile_error;
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
    /* И9.7: настройка таймаута СЫРОЙ строкой. Нужна, чтобы решить, чьё
     * значение важнее — профиля или человека, — по самому факту, что
     * человек что-то написал, а не по результату разбора: «0» и мусор
     * тоже написанное, и подменять их значением профиля молча нельзя. */
    std::string llm_timeout_setting;

    /* Настройки агента (5.3) — переопределяют дефолты из core/limits.h. */
    int max_steps = 12;            // лимит шагов ReAct на задачу (kMaxSteps)
    size_t session_budget = 60000; // бюджет символов истории сессии (kSessionBudget)

    /* И8.8: насколько глубоко можно вкладывать `task`.
     *
     * Настройка `wp_coder.subagent_depth`, дефолт — константа
     * limits::kSubagentDepthLimit, и она же единственный её источник:
     * предел, о котором думают в двух местах, становится двумя разными
     * числами (плановый дефолт у max_steps — историческая опечатка, а не
     * образец).
     *
     * Проверяет его инструмент `task` (subagent.cpp), потому что ответ на
     * вопрос «кто сейчас выполняет ход» лежит в RunScope::depth. Само
     * ограничение написано в И8.7 вместе со счётчиком (отклонение №78):
     * без него `task` внутри `task` — рекурсивный генератор запросов к
     * модели, и ждать настройки для этого было бы оставлять инструмент
     * открытым на то время, пока пишется его удобство. */
    int subagent_depth = limits::kSubagentDepthLimit;

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

    /* A1: последние вызовы инструментов (fingerprint) для детекта зацикливания.
     * Хода РОДИТЕЛЯ: у вложенного хода свой счётчик (RunScope::loop_calls,
     * И8.7), иначе вызовы субагента выглядели бы вызовами родителя. */
    std::deque<std::string> recent_calls;

    /* И8.7: кто выполняет текущий ход — сессия или субагент. */
    RunScope scope;

    /* И8.14: задачи, поставленные в фон (`task` с background: true), и
     * счётчик цепочки автоматических ходов.
     *
     * Очередь разбирается МЕЖДУ ходами родителя (worker_main), а не в
     * своём потоке: область выполнения одна на весь движок (RunScope), и
     * фоновый ход в своём потоке отдал бы родителю enforcement по правилам
     * РЕБЁНКА. Обоснование выбора — в core/subagent.h.
     *
     * background_turns — сколько автоматических ходов подряд уже начато
     * по результатам фоновых задач. Сбрасывается в submit(), то есть любой
     * запрос человека обнуляет цепочку; ограничение — limits::
     * kBackgroundTurnLimit, без него модель, ставящая задачи в фон на
     * каждом ходу, гоняла бы платные запросы без спроса. */
    std::deque<SubagentJob> background_tasks;
    int background_turns = 0;

    /* И4.6: план задачи. Сбрасывается при новой задаче (run_task) — список
     * прошлой задачи в промпте новой только сбивает. */
    std::vector<TodoItem> todos;

    /* B2: кэш repo_map на текущую задачу — не перечитываем структуру проекта
     * на каждом шаге, если корень не менялся. Сбрасывается в начале run_task. */
    std::string repo_map_cache_root;
    std::string repo_map_cache_result;

    /* И10.1: снимок рабочего каталога, снятый в начале последнего шага.
     *
     * Одно поле, а не стек: стек уровней сессии пришёл отдельной задачей
     * (10.4) и живёт в undo_stack, потому что «снимок последнего шага» и
     * «уровни, куда можно вернуться» — разные вещи с разными сроками
     * жизни (см. границу 4 в шапке core/snapshot.h).
     *
     * Снимок снимается ВНЕ state_.mtx и кладётся под локом: git в
     * худшем случае берёт kSnapshotTimeoutSec, а UI ждёт тот же
     * мьютекс — держать его на git значило бы вернуть тот самый
     * висящий GUI, который закрыл D1.
     *
     * snapshots_taken считает удачные снимки, а не вызовы take(): по
     * нему видно, снимает ли агент вообще, не читая журнала git.
     * Ошибка последнего снимка — в Snapshot::reason, а не в отдельном
     * поле: она относится к тому же снимку. */
    snapshot::Snapshot last_snapshot;
    int snapshots_taken = 0;

    /* И10.4: уровни сессии, к которым можно вернуться (undo/redo).
     *
     * Пишет его только рабочий поток — цикл, субагент и инструменты
     * агента; UI стек не трогает. Инструменты берут его копией под локом
     * и кладут обратно (см. шапку core/snapshot.h: внутри undo/redo ходят
     * git и файловая система, а лок на них — это висящий GUI).
     *
     * Обнуляется в двух местах, и оба названы в шапке: смена проекта
     * (сверкой по полю снимка, см. UndoStack::push) и кнопка «Очистить
     * сессию» (Engine::clear_session).
     *
     * А вот НОВАЯ ЗАДАЧА (Engine::submit) стек НЕ обнуляет — и это
     * решение, а не пропуск: уровень — это состояние каталога проекта, а
     * не состояние разговора, и «отмени последнее» в новой задаче
     * означает отменить последнее сделанное с файлами, а не «то, что
     * было до начала этой задачи». Цена названа: стек живёт дольше одной
     * задачи, то есть отменой можно вернуться через границу задач, и
     * модель, жаловавшаяся на «отменить» в новой задаче, получит отказ
     * с названной причиной, а не тишину. */
    snapshot::UndoStack undo_stack;
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

    /* И10.5: что покажет отмена уровня `index` стека сессии.
     *
     * Отдельный метод, а не `state().undo_stack.diff_step(...)` вызовом из
     * UI, по двум причинам, и обе про блокировки: снимки читаются под
     * `state_.mtx`, а считается diff ВНЕ лока (git держит до
     * kSnapshotTimeoutSec, а UI ждёт тот же мьютекс — это тот же
     * висящий UI, что закрыл D1). Метод делает ровно это и больше
     * ничего: копия уровней под локом, подсчёт вне.
     *
     * index — индекс уровня, отмена которого вернёт проект в его
     * состояние, то есть на единицу больше позиции (`position()`).
     *
     * Стоимость названа, потому что её платит вызывающий: на проекте под
     * git это одна команда git, а на проекте БЕЗ git верхний уровень
     * сравнивается с текущим состоянием каталога, и ради этого снимок
     * копирует проект целиком. Нажатие кнопки в окне — это единичная
     * операция человека, а не шаг агента.
     *
     * Ничего не меняет: ни стека, ни файлов проекта. */
    snapshot::DiffReport level_diff(size_t index);

    /* Resume сессии (5.2): сохранение/загрузка диалога на диск (И5.6).
     * Файл: <data_dir>/wp_coder/sessions/<session_id>.json.
     *
     * Формат и атомарная запись — в core/session_store.h; здесь только
     * перевод между историей движка и файлом. */
    void save_session();
    void load_session();

    /* Идентификатор сессии, выдавая его при первом обращении (И8.9).
     *
     * Нужен не только сохранению: дочерняя сессия субагента ссылается на
     * родителя (`parent_id`), и ссылка на пустую строку была бы не
     * ссылкой. Идентификатор выдаётся здесь, а запись файла — там, где
     * файл и нужен, поэтому у сессии, которая ещё ни разу не
     * сохранялась, идентификатор есть, а файла нет: это одно и то же
     * «кто я», а не два разных. */
    std::string ensure_session_id();

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

    /* Сборка полного системного промпта.
     *
     * И8.7: промпт и набор видимых инструментов берутся у ТЕКУЩЕГО
     * агента (RunScope). У хода сессии агента нет — базовый промпт и
     * правила сессии, как раньше; у вложенного хода субагента — его
     * роль и его правила. Один путь сборки на обоих, а не две функции
     * с почти одинаковым текстом.
     */
    std::string build_system_prompt() const;

    /* Копия текущей области выполнения — для вызывающего, который
     * решает, кем быть (инструмент `task`, И8.7), и для проверок. */
    RunScope scope_snapshot() const;

    /* И9.1: прочитать инструкции, если они ещё не прочитаны.
     *
     * Загрузка ЛЕНИВАЯ и происходит на worker-потоке, а не в
     * load_settings: там она повесила бы UI на время сетевого запроса
     * (ll_plugin_init зовёт Engine::init на хостовом потоке), а здесь
     * ожидание приходится на начало хода, где оно и уместно.
     *
     * Повторно НЕ перечитывает: invalidate_prompt_cache() зовётся на
     * каждом todowrite, и перечитывание означало бы сетевой запрос
     * посреди хода. Перечитать явно можно только reload_instructions —
     * при смене корня проекта.
     *
     * Свой мьютекс, а не state_.mtx: загрузка ходит по диску и в сеть,
     * а state_.mtx общий с UI, и держать его на этом времени нельзя
     * (тот же довод, что у compact_history_if_needed). Порядок
     * блокировок — сначала этот, потом state_.mtx; обратного места в
     * коде нет, и добавлять его нельзя. */
    void ensure_instructions() const;

    /* Сбросить прочитанные инструкции, чтобы следующая сборка промпта
     * прочитала их заново. Зовётся из UI при смене корня проекта и из
     * load_settings при перечитывании настроек. */
    void reload_instructions() const;

    /* И9.2: прикрепить инструкции, найденные рядом с прочитанным файлом.
     *
     * Список пополняется МЕЖДУ запросами к модели (инструмент `read`
     * вызывается между ними), поэтому кэш промпта сбрасывается здесь: иначе
     * правила каталога нашлись бы, но до модели не дошли бы — она увидела
     * бы их только в следующем ходе, а если ход последний, то и вовсе
     * никогда.
     *
     * Публичная — по той же причине, что deliver_background_tasks():
     * проверка обязана доходить до места, где это происходит, а поднять
     * worker-поток в тесте нельзя. */
    void attach_instructions(std::vector<Instruction> more) const;

    /* Список источников инструкций — для проверок и диагностики.
     * Копией, а не ссылкой: ссылка пережила бы освобождение лока, под
     * которым она взята, и читала бы список, который уже переписали. */
    std::vector<Instruction> instructions_for_test() const {
        std::lock_guard<std::mutex> lk(state_.mtx);
        return state_.instructions;
    }

    /* Инвалидация кэша промпта (вызывать при изменении настроек). */
    void invalidate_prompt_cache() const { state_.prompt_dirty = true; }

    /* --- Профиль harness (И9.7) --- */

    /* Каталог с ПОСТАВЛЕННЫМИ профилями (profiles/wp_coder плагина).
     * Ставится из src/plugin_main.cpp: путь к дереву плагина известен
     * только там, и это ровно тот же приём, что с WP_CODER_SKILLS_DIR.
     * Ядро получает готовую строку и ни о каком каталоге плагина не
     * знает — иначе проверка «профиль не найден» зависела бы от того,
     * откуда собраны тесты. */
    void set_profiles_bundled_dir(std::string dir);

    /* Профиль по имени ИМЕНИ АГЕНТА (`profile:` во frontmatter, И9.7).
     *
     * Отдельная функция, а не поле в AgentDef: правила агента собирает
     * владелец правил (Info, И8.4), и он же должен получить готовый
     * профиль — иначе решение «чем этот агент ограничен» принималось бы
     * в инструменте `task` и в UI отдельно.
     *
     * nullptr — профиль не задан или не читается; это НЕ ошибка сама по
     * себе, и why (необязательный) получает причину. */
    std::shared_ptr<const harness::Profile> profile_for_agent(
        const std::string& agent_profile_name, std::string* why = nullptr) const;

    /* Профиль сессии: разобран или нет, и почему. Для панели и лога. */
    bool session_profile(harness::Profile* out, std::string* error) const;

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

    /* Аварийная обрезка истории по бюджету СИМВОЛОВ (trim_history_if_needed).
     *
     * Аварийная — потому что после И7.10 это НЕ основной путь сжатия:
     * обрезка по строкам не оставляет модели ничего, кроме первых строк
     * каждого сообщения, и ход работы после неё восстановить нельзя. Она
     * осталась страховкой на два случая: окно переполнилось, а сжатие не
     * справилось (сводщик не ответил, сводить нечего), и история растёт
     * быстрее локального бюджета символов при неизвестном окне модели.
     *
     * Единственная безопасная точка входа: state_.mtx берётся ровно один
     * раз внутри. Вызывать из кода, который уже держит лок, нельзя —
     * мьютекс нерекурсивный (engine.h), повторный захват вешает GUI
     * (D1). Правило обрезки — в compress_history (core/message.h): там
     * текст, а здесь только лок и бюджет. */
    void trim_history_if_needed();

    /* Автосжатие по порогу окна (И7.10): сводка отдельным агентом вместо
     * обрезки строк.
     *
     * Порядок (каждый шаг виден пользователю событием):
     *   1. прореживание вывода инструментов, если его разрешено (И7.9) —
     *      бесплатно и часто одного этого хватает;
     *   2. если окно переполнено — сводка (select_to_compact + summarize +
     *      compacted_history), и новая история записывается в сессию;
     *   3. если сводка не вышла или вышла не в то место — аварийная
     *      обрезка, с честным объяснением пользователю.
     *
     * turn — запрос к модели для сводки. Приходит снаружи, а не
     * собирается здесь: у движка нет способа узнать ответ модели в тесте,
     * а проверять тут надо именно решение (что сжалось, что осталось,
     * что сказано пользователю), а не сеть.
     *
     * Тот же запрет на лок: state_.mtx берётся короткими захватами, а
     * ЗАПРОС К МОДЕЛИ идёт без него (тот же D1).
     */
    void compact_history_if_needed(const compaction::SummaryTurn& turn);

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

    /* И8.14: разбор очереди фоновых задач и доставка их результатов.
     *
     * Зовёт worker_main после КАЖДОГО хода родителя (engine.cpp): ходов,
     * поставивших задачи в фон, не знает никто, кроме движка. Функция
     * выполняет поставленные задачи по одной, кладёт их результаты в
     * сессию ОДНИМ синтетическим сообщением и, если результаты есть,
     * начинает ход, в котором модель их прочитает.
     *
     * Публичная — по той же причине, что session_for_test() и
     * trim_session_test(): проверка обязана доходить до места, где это
     * происходит, а воркер-поток в тесте не поднять. */
    void deliver_background_tasks();

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
    /* И9.1: свой лок загрузки инструкций. Отдельный от state_.mtx
     * (см. ensure_instructions) и по той же причине — загрузка не должна
     * держать общий лок, пока ходит по диску и в сеть. */
    mutable std::mutex instructions_mtx_;

    void run_task(std::string task);
    void worker_main();

    /* И9.7: применить профиль harness к сессии — таймаут, команды,
     * запреты ключей. Зовётся из load_settings ПОСЛЕ
     * permissions_.load_user_rules: запреты профиля кладутся в конец
     * набора и потому перекрывают правила пользователя (тот же порядок,
     * что у запретов сессии в Info, И8.10).
     *
     * Отдельный метод, а не строки в load_settings: он обязан быть
     * вызываемым ПОВТОРНО без перечитывания всех настроек, иначе
     * проверка «профиль сменился — граница снята» требовала бы
     * поддельной таблицы настроек. */
    void apply_session_profile();

    /* Путь к файлу сохранённой сессии (resume, 5.2): <data_dir>/wp_coder/session.json. */
    std::string session_file_path() const;
};

/* --- И8.7: подмена области выполнения на время вложенного хода ---
 *
 * Без этого guard'а субагент оставил бы после себя чужой промпт и
 * скрытые инструменты: Engine и ToolsRegistry — синглтоны процесса, а
 * область меняется в общем состоянии. Возврат области и сброс кэша
 * промпта происходят в деструкторе, потому что «забыть вернуть» на
 * пути любого отказа (исключение, ранний return) — это ровно тот
 * случай, из-за которого следующий ход пошёл бы с промптом субагента.
 */
class ScopedAgentScope {
public:
    ScopedAgentScope(Engine& engine, RunScope next);
    ~ScopedAgentScope();

    ScopedAgentScope(const ScopedAgentScope&) = delete;
    ScopedAgentScope& operator=(const ScopedAgentScope&) = delete;

private:
    Engine& engine_;
    RunScope previous_;
};

/* Удобные глобальные accessor-ы. */
inline Engine& engine() { return Engine::instance(); }
inline EngineState& engine_state() { return Engine::instance().state(); }

} // namespace coder