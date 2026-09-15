#pragma once

/*
 * agent_components.h — Компоненты ReAct-цикла (Фаза D1).
 *
 * Монолитный Engine::run_task разбит на независимые классы:
 *   SessionStore   — история сессии (add/trim/snapshot)
 *   PermissionGate — разрешения на доступ к файлам
 *   ToolRunner     — запуск инструментов с валидацией и guard
 *   Planner        — планирование (B1)
 *   AgentLoop      — основной ReAct-цикл
 *
 * Используем AgentEvent::Kind из engine.h для событий.
 */

#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <functional>

#include "engine.h"

namespace coder {

/* Callback для событий агента (лог в UI). */
using AgentEventCallback = std::function<void(AgentEvent::Kind, const std::string&)>;

/* Управляет историей многоходовой сессии агента. */
class SessionStore {
public:
    explicit SessionStore(EngineState& state);

    void clear();
    void push_user(const std::string& content);
    void push_assistant(const std::string& content);
    std::vector<ChatMsg> snapshot() const;
    bool empty() const;
    void trim();
    std::vector<ChatMsg> messages() const;

private:
    EngineState& state_;
};

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

/* Выполняет инструменты: валидация, guard против зацикливания, запуск. */
class ToolRunner {
public:
    ToolRunner(EngineState& state, HostCallbacks& cb,
               AgentEventCallback push_event)
        : state_(state), cb_(cb), push_event_(std::move(push_event)) {}

    std::string run(const std::string& tool_name, const ToolArgs& args);
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

/* Основной ReAct-цикл агента. */
class AgentLoop {
public:
    AgentLoop(EngineState& state, HostCallbacks& cb,
              AgentEventCallback push_event)
        : state_(state), cb_(cb), push_event_(std::move(push_event)) {}

    bool run(const std::string& sys_prompt, std::string& full_response);

private:
    EngineState& state_;
    HostCallbacks& cb_;
    AgentEventCallback push_event_;
};

} // namespace coder
