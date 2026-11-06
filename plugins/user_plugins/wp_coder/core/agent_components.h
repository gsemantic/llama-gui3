#pragma once

/*
 * agent_components.h — Компоненты ReAct-цикла (Фаза D1, И5.7).
 *
 * Монолитный Engine::run_task разбит на независимые классы:
 *   PermissionGate — разрешения на доступ к файлам
 *   ToolRunner     — запуск инструментов с валидацией и guard
 *   Planner        — планирование (B1)
 *   AgentLoop      — основной ReAct-цикл
 *
 * Используем AgentEvent::Kind из engine.h для событий.
 *
 * И5.7: класса SessionStore здесь больше нет, и это не переименование.
 * Он управлял историей как списком строк, а историей стал список
 * сообщений с частями (core/message.h) — и методы «снимок/сообщения/
 * обрезка» превратились в свободные функции над std::vector<Message>
 * (find_message, model_history_chars, compress_history). Класс остался
 * бы оболочкой над одной строкой, а состояние у него и так одно —
 * EngineState, который и так общий.
 */

#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <functional>

#include "engine.h"
#include "llm_event.h"
#include "message.h"

namespace coder {

/* Callback для событий агента (лог в UI). */
using AgentEventCallback = std::function<void(AgentEvent::Kind, const std::string&)>;

/* Управляет разрешениями на доступ к файлам за пределами проекта. */
class PermissionGate {
public:
    PermissionGate(EngineState& state, HostCallbacks& cb,
                   AgentEventCallback push_event)
        : state_(state), cb_(cb), push_event_(std::move(push_event)) {}

    std::string check(const std::string& abs_path);
    void allow_once(const std::string& path);
    void allow_always(const std::string& path);
    void reject();
    bool is_waiting() const;
    void wait(std::unique_lock<std::mutex>& lk);

private:
    EngineState& state_;
    HostCallbacks& cb_;
    AgentEventCallback push_event_;
};

/* Исход вызова инструмента: результат ИЛИ отказ (И5.7).
 *
 * Раньше ToolRunner::run возвращал std::string, и ToolOutput (И4.10)
 * терялся по дороге: заголовок, метаданные и признак усечения доходили
 * только до UI, а в историю попадала голая строка. Часть сообщения
 * хранит ToolOutput целиком (core/message.h), поэтому наружу нужен и
 * результат, и отказ — а «пустой вывод» как признак отказа не годится:
 * инструмент может закончиться успехом и не напечатать ничего. */
struct ToolOutcome {
    bool ok = false;              /* true — результат, false — отказ */
    ToolOutput output;
    std::string error;
};

/* Выполняет инструменты: enforcement режима, guard против зацикливания,
 * валидация аргументов по схеме, запуск. */
class ToolRunner {
public:
    ToolRunner(EngineState& state, HostCallbacks& cb,
               AgentEventCallback push_event)
        : state_(state), cb_(cb), push_event_(std::move(push_event)) {}

    /* Основной путь И1: типизированные аргументы.
     *
     * Исторью НЕ занимается: результат возвращается вызывающему, а в
     * сессию его кладёт цикл, у которого есть идентификатор сообщения и
     * вызова. Раньше неизвестный инструмент писал в сессию сам, а цикл
     * писал туда же вторую строку — то есть один и тот же отказ
     * попадал в историю дважды, и модель видела его как два результата. */
    ToolOutcome run(const std::string& tool_name, const json::JsonValue& args);
    bool is_loop_guard(const std::string& fingerprint) const;
    void record_call(const std::string& fingerprint);

private:
    EngineState& state_;
    HostCallbacks& cb_;
    AgentEventCallback push_event_;
};

/* Планирование (B1): первый LLM-вызов — план действий перед исполнением. */
class Planner {
public:
    Planner(EngineState& state, HostCallbacks& cb,
            AgentEventCallback push_event)
        : state_(state), cb_(cb), push_event_(std::move(push_event)) {}

    bool plan(const std::string& sys_prompt);

private:
    EngineState& state_;
    HostCallbacks& cb_;
    AgentEventCallback push_event_;
};

/* Ответ пользователя на вопрос о внешнем пути (И5.7).
 *
 * Отдельный тип, а не bool: «разрешено» и «отказано» — противоположные
 * исходы, и путать их нельзя. Источник ответа — список разрешённых
 * путей (см. permission_outcome), а не флаг: флаг был бы вторым местом
 * одного и того же факта. */
enum class PermissionOutcome { Granted, Rejected };

/* Что на самом деле решил пользователь (И5.7).
 *
 * Список разрешённых путей и есть ответ: allow_once/allow_always кладут
 * путь туда, reject() — нет. Отдельный флаг означал бы второе место, где
 * живёт один и тот же факт, и места разъедутся.
 *
 * Лок берётся внутри: вызывающий цикла лока НЕ держит (state_.mtx
 * нерекурсивный). */
PermissionOutcome permission_outcome(EngineState& state, const std::string& path);

/* И10.1: снимок рабочего каталога в начале шага.
 *
 * Отдельная функция, а не метод AgentLoop: шаг есть и у основного
 * цикла, и у вложенного хода субагента (run_subagent_turn), а тело
 * снимка — одно на оба. Вторая копия означала бы, что одна из двух
 * точек рано или поздно перестанет снимать (Д2).
 *
 * Кто зовёт и когда — в core/snapshot.h: в нашем цикле начало шага и
 * начало хода — одно и то же место (И5.7: за итерацию создаётся ровно
 * одно сообщение ассистента). Планирование (Planner::plan) снимка не
 * берёт: инструментов оно не зовёт и файлов не меняет.
 *
 * Возвращаются ОБА снимка, и это не удобство, а страховка от тихой
 * потери: снимок, который лежал в состоянии, затирается новым, и
 * сравнивать «до и после» (И10.2) пришлось бы читать состояние ДО
 * вызова — а тот, кто это читает, однажды прочитает ПОСЛЕ и получит
 * «до и после совпали», то есть пустой список изменений при полном
 * списке сделанного. Первая версия возвращала один снимок, и это
 * поймала проверка склейки, а не чтение кода.
 *
 * Неудача не молчит, но и не кричит на каждом шаге: событие
 * уходит, только если причина ИЗМЕНИЛАСЬ. Иначе один и тот же отказ
 * git на двенадцати шагах засыпал бы ленту событий двенадцатью
 * одинаковыми строками — и человек, читающий её, пропустил бы всё
 * остальное. */
struct StepSnapshot {
    snapshot::Snapshot before;  /* что лежало в состоянии до вызова */
    snapshot::Snapshot after;   /* только что снятое */
    /* Копии, которые снял этот вызов и которые больше не читает НИКТО:
     * снимок, не ставший уровнем (шаг ничего не изменил). На проекте без
     * git это полная копия каталога, и без уборки она осталась бы на
     * диске до конца сессии, а удалять её должен вызывающий — потому что
     * `after` ещё нужен ему для сравнения «до и после» (changed_files).
     * Уборка внутри take_step_snapshot оказалась бы слишком ранней и
     * сломала бы именно этот список: чтение удалённого каталога молча
     * даёт «изменений нет». */
    std::vector<snapshot::Snapshot> discard;
};

StepSnapshot take_step_snapshot(EngineState& state,
                                HostCallbacks& cb,
                                AgentEventCallback push_event);

/* И10.2: список файлов, изменившихся между снимком предыдущего шага и
 * текущим, — в то сообщение, которому эти изменения принадлежат.
 *
 * Именно в ПРЕДЫДУЮЩЕЕ: снимок снят в начале шага, то есть до того, как
 * агент что-либо сделал на этом шаге, и разница между ним и текущим
 * состоянием — это ровно то, что изменил предыдущий ход. Часть с этим
 * списком — то, по чему 10.3 вернёт проект назад.
 *
 * Одна функция на оба цикла (основной и вложенный ход субагента): места
 * подключения разные, а решение одно, и две копии разошлись бы при
 * первой же правке (Д2). Работает с вектором сообщений, а не с
 * EngineState, поэтому вызывающий сам решает, под каким локом это
 * делать.
 *
 * Сравнение снимков принимается ГОТОВЫМ (`changes`), а не считается
 * здесь: `snapshot::changed_files` зовёт git, а держать `state_.mtx` на
 * git — это ровно тот висящий UI, который закрыл D1 (и который сам
 * же снимок 10.1 снимает вне лока). Последовательность у вызывающего:
 * взять `after` под локом → посчитать `changes` без лока → положить
 * часть под локом.
 *
 * Возвращает сообщение, в которое легла часть, либо nullptr — когда
 * записывать нечего. Различить «сравнение не состоялось» и «ход ничего
 * не менял» по nullptr нельзя, и по договорённости nullptr значит
 * «части нет»: пустой список файлов — это «ход ничего не менял», а
 * такую правку хранить незачем. */
Message* attach_patch_part(std::vector<Message>& history,
                           const snapshot::Snapshot& before,
                           const snapshot::FileChanges& changes);

/* Основной ReAct-цикл агента. */
class AgentLoop {
public:
    AgentLoop(EngineState& state, HostCallbacks& cb,
              AgentEventCallback push_event)
        : state_(state), cb_(cb), push_event_(std::move(push_event)) {}

    bool run(const std::string& sys_prompt, std::string& full_response);

private:
    /* Реплика к модели, которую ввёл не пользователь, а цикл: напоминание
     * «продолжай», «инструменты больше нельзя». Кладётся в историю
     * сообщением пользователя — так её видела модель и раньше. */
    void append_note(const std::string& text);

    /* Форсированный итоговый ход: напоминание в историю, вопрос модели,
     * её ответ — в историю и в результат задачи.
     *
     * Одна функция на оба случая (застревание и исчерпание шагов): они
     * устроены одинаково, и раньше это был один и тот же код,
     * скопированный дважды. Возвращает false, если модель не ответила. */
    bool ask_for_summary(const std::string& sys_prompt,
                         const std::string& reminder,
                         std::string& full_response);

    EngineState& state_;
    HostCallbacks& cb_;
    AgentEventCallback push_event_;
};

/* --- Вложенный ход субагента (И8.7, порт tool/task.ts) ---
 *
 * Отдельная функция, а не AgentLoop с флагом: контракты разные.
 * Родитель работает с СЕССИЕЙ (state_.session, Planner, сжатие,
 * ожидание разрешения, состояние движка), субагент — со своей
 * короткой историей, которая никуда не сохраняется. Общая механика
 * (fetch, fold, ToolRunner::run, cap_result, sync_tool_parts,
 * учёт токенов) — та же; своя политика и своё условие завершения.
 * Обоснование целиком — в комментарии у реализации.
 *
 * Кто выполняет ход — НЕ аргумент: область (RunScope) уже подменена
 * вызывающим (ScopedAgentScope), и enforcement спрашивает именно её.
 * Иначе «кто я» поехало бы вторым путём и разошлось бы с тем, что
 * видит ToolRunner.
 *
 * Прерывание: между шагами. Каскад отмены родителя → cancel(child) с
 * живым запросом — И8.12; здесь готовится только точка, где он будет
 * подключён (между шагами, а не внутри). */
struct SubagentResult {
    bool ok = false;
    std::string text;    /* итог субагента — во что обернётся task */
    std::string error;   /* причина; ok == false */
    int steps = 0;       /* сколько шагов реально сделано */
    /* История ребёнка после хода (И8.9) — её записывает вызывающий в
     * дочернюю сессию. Возвращается ЛЮБЫМ исходом, включая отказ и
     * прерывание: частично сделанная работа — это тоже работа, и
     * продолжать её (`task_id`) полезнее, чем начинать заново.
     *
     * Именно поэтому у функции один выход, а не return в шести местах:
     * «история на выходе» обеспечена структурой, а не дисциплиной. */
    std::vector<Message> history;
};

/* Ход субагента (И8.7).
 *
 * seed — начальная история ребёнка. Пустая у новой задачи; у
 * продолжения (`task_id`, И8.9) — история из дочерней сессии, и тогда
 * prompt становится ПРОДОЛЖЕНИЕМ разговора, а не первым сообщением.
 *
 * Корень цепочки (`parent_id` ходов) — всегда первое сообщение истории,
 * а не последнее: при продолжении это корень исходной задачи, иначе в
 * одном файле оказалось бы два разговора (тот же довод, что у сессии в
 * И5.4). */
SubagentResult run_subagent_turn(EngineState& state, HostCallbacks& cb,
                                 AgentEventCallback push_event,
                                 const std::string& sys_prompt,
                                 const std::string& prompt, int max_steps,
                                 std::vector<Message> seed = {});


} // namespace coder
