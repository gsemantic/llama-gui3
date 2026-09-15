// agent_components.cpp (Фаза D1) — реализация компонентов без goto

#include "agent_components.h"
#include "prompts.h"
#include "shell.h"
#include "engine.h"
#include "limits.h"

#include <sstream>
#include <vector>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <fstream>
#include <filesystem>

namespace coder {

/* Лимиты ReAct-цикла — единый источник: core/limits.h (Фаза 4.5).
 * kMaxSteps/kSessionBudget переопределяются настройками агента (5.3):
 * state_.max_steps / state_.session_budget. */
using limits::kResultBudget;

/* Порог «застревания»: если N шагов подряд модель даёт короткий
 * ответ (< 200 символов), считаем, что она не может прогрессировать
 * и запрашиваем итоговый ответ. 200 — потому что ответ 100-190 символов
 * тоже слишком короткий для продуктивного шага агента. */
constexpr int kStuckThreshold = 3;
constexpr size_t kShortResponseLen = 200;

/* ======================================================================
 * SessionStore
 * ====================================================================== */

SessionStore::SessionStore(EngineState& state) : state_(state) {}

void SessionStore::clear() {
    std::lock_guard<std::mutex> lk(state_.mtx);
    state_.session.clear();
}

void SessionStore::push_user(const std::string& content) {
    std::lock_guard<std::mutex> lk(state_.mtx);
    state_.session.push_back({"user", content});
}

void SessionStore::push_assistant(const std::string& content) {
    std::lock_guard<std::mutex> lk(state_.mtx);
    state_.session.push_back({"assistant", content});
}

std::vector<ChatMsg> SessionStore::snapshot() const {
    std::lock_guard<std::mutex> lk(state_.mtx);
    return state_.session;
}

bool SessionStore::empty() const {
    std::lock_guard<std::mutex> lk(state_.mtx);
    return state_.session.empty();
}

std::vector<ChatMsg> SessionStore::messages() const {
    std::lock_guard<std::mutex> lk(state_.mtx);
    return state_.session;
}

void SessionStore::trim() {
    std::lock_guard<std::mutex> lk(state_.mtx);
    auto& s = state_.session;
    size_t total = 0;
    for (const auto& m : s) total += m.content.size();
    if (total <= state_.session_budget) return;

    auto make_stub = [](const std::string& content) -> std::string {
        std::string c = content;
        std::string tool;
        size_t colon = c.find(']');
        if (c.rfind("RESULT [", 0) == 0 && colon != std::string::npos) {
            tool = c.substr(8, colon - 8);
        }
        std::string first;
        size_t nl = c.find('\n');
        if (nl != std::string::npos && nl + 1 < c.size()) {
            first = c.substr(nl + 1, std::min<size_t>(60, c.size() - nl - 1));
            size_t nl2 = first.find('\n');
            if (nl2 != std::string::npos) first.resize(nl2);
        }
        if (first.empty()) first = "(без вывода)";
        std::string r = "[RESULT " + tool + " сжат]: " + first;
        if (r.size() > 90) { r.resize(90); r += "…"; }
        return r;
    };

    for (size_t i = 1; i < s.size(); ++i) {
        if (total <= state_.session_budget) break;
        if (s[i].role != "user") continue;
        if (s[i].content.rfind("RESULT [", 0) != 0) continue;
        std::string stub = make_stub(s[i].content);
        total -= s[i].content.size();
        s[i].content = stub;
        total += stub.size();
    }

    size_t last = s.size() > 0 ? s.size() - 1 : 0;
    for (size_t i = 1; i < s.size() && i < last; ++i) {
        if (total <= state_.session_budget) break;
        if (s[i].role != "assistant") continue;
        std::string stub;
        if (s[i].content.rfind("[ПЛАН]", 0) == 0) stub = "[ПЛАН (счат)]";
        else {
            stub = s[i].content.substr(0, std::min<size_t>(60, s[i].content.size()));
            if (s[i].content.size() > 60) stub += "…";
        }
        total -= s[i].content.size();
        s[i].content = stub;
        total += stub.size();
    }
}

/* ======================================================================
 * PermissionGate
 * ====================================================================== */

std::string PermissionGate::check(const std::string& abs_path) {
    std::string proj;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        proj = state_.project_dir;
    }
    if (!is_path_outside(abs_path, proj)) return "";
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        if (is_path_allowed(abs_path, proj, state_.allowed_external_paths)) return "";
    }

    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.pending_permission_path = abs_path;
        state_.state = AgentState::WaitingPermission;
    }
    this->push_event_(AgentEvent::Tool, "[access] Требуется разрешение: " + abs_path);
    return "[ВАЖНО] Доступ запрещён. Файл вне проекта: " + abs_path
           + "\nНЕ ПОВТОРЯЙ вызов. Скажи пользователю что нужно нажать «Разрешить»."
           + "\nЖди подтверждения. Не пытайся снова до подтверждения.";
}

void PermissionGate::allow_once(const std::string& path) {
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.allowed_external_paths.push_back(path);
        state_.once_path = path;
        state_.pending_permission_path.clear();
        state_.state = AgentState::Executing;
    }
    state_.permission_cv.notify_all();
}

void PermissionGate::allow_always(const std::string& path) {
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.allowed_external_paths.push_back(path);
        state_.pending_permission_path.clear();
        state_.state = AgentState::Executing;
    }
    state_.permission_cv.notify_all();
    std::string json = "[";
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        for (size_t i = 0; i < state_.allowed_external_paths.size(); ++i) {
            if (i > 0) json += ",";
            json += "\"" + state_.allowed_external_paths[i] + "\"";
        }
    }
    if (cb_.settings_set) cb_.settings_set("wp_coder.allowed_external_paths", json);
}

void PermissionGate::reject() {
    std::lock_guard<std::mutex> lk(state_.mtx);
    state_.pending_permission_path.clear();
    state_.state = AgentState::Executing;
    state_.permission_cv.notify_all();
}

bool PermissionGate::is_waiting() const {
    std::lock_guard<std::mutex> lk(state_.mtx);
    return state_.state == AgentState::WaitingPermission;
}

void PermissionGate::wait(std::unique_lock<std::mutex>& lk) {
    state_.permission_cv.wait(lk, [this] {
        return state_.state != AgentState::WaitingPermission
            || state_.shutting_down || state_.abort_requested.load();
    });
}

/* ======================================================================
 * ToolRunner
 * ====================================================================== */

std::string ToolRunner::run(const std::string& tool_name, const ToolArgs& args) {
    if (!ToolsRegistry::instance().has(tool_name)) {
        std::string unknown = "[ошибка] неизвестный инструмент: " + tool_name
            + "\nДоступные инструменты: " + ToolsRegistry::instance().join_tools();
        this->push_event_(AgentEvent::Error, (tool_name + " — неизвестный инструмент").c_str());
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            state_.session.push_back({"user", "RESULT [" + tool_name + "]:\n" + unknown});
        }
        return unknown;
    }

    std::string fp = tool_name + "\n" + args.path + "\n" + args.root + "\n"
        + args.query + "\n" + args.pattern + "\n" + args.cli + "\n" + args.url + "\n"
        + std::to_string(args.k);

    bool loop_detected = false;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        if (state_.recent_calls.size() >= 2 &&
            state_.recent_calls[state_.recent_calls.size() - 1] == fp &&
            state_.recent_calls[state_.recent_calls.size() - 2] == fp) {
            loop_detected = true;
            state_.recent_calls.clear();
        } else {
            state_.recent_calls.push_back(fp);
            if (state_.recent_calls.size() > 8) {
                state_.recent_calls.pop_front();
            }
        }
    }  /* mtx отпущен — push_event_ безопасен */
    if (loop_detected) {
        this->push_event_(AgentEvent::Error,
            "Инструмент " + tool_name + " вызван 3 раза подряд с одинаковыми "
            "аргументами — прерываю, чтобы не зациклиться.");
        return "[ошибка] зацикливание вызова " + tool_name;
    }

    std::string result = ToolsRegistry::instance().run(tool_name, args);
    this->push_event_(AgentEvent::Tool, tool_name + " -> " + result);
    return result;
}

bool ToolRunner::is_loop_guard(const std::string& fingerprint) const {
    std::lock_guard<std::mutex> lk(state_.mtx);
    if (state_.recent_calls.size() >= 2 &&
        state_.recent_calls[state_.recent_calls.size() - 1] == fingerprint &&
        state_.recent_calls[state_.recent_calls.size() - 2] == fingerprint) {
        return true;
    }
    return false;
}

void ToolRunner::record_call(const std::string& fingerprint) {
    std::lock_guard<std::mutex> lk(state_.mtx);
    state_.recent_calls.push_back(fingerprint);
    if (state_.recent_calls.size() > 8) {
        state_.recent_calls.pop_front();
    }
}

/* ======================================================================
 * Planner
 * ====================================================================== */

bool Planner::plan(const std::string& sys_prompt) {
    if (!state_.use_planning) return false;

    /* Если в сессии уже есть план — не генерируем новый.
     * Раньше каждый submit() очищал сессию и план терялся,
     * теперь сессия сохраняется между запросами. */
    bool plan_exists = false;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        for (const auto& msg : state_.session) {
            if (msg.role == "assistant" &&
                msg.content.rfind("[ПЛАН]", 0) == 0) {
                plan_exists = true;
                break;
            }
        }
    }  /* mtx отпущен ДО push_event_ — иначе deadlock: push_event_
        * внутри тоже захватывает state_.mtx, а std::mutex не рекурсивный. */
    if (plan_exists) {
        std::cerr << "[wp_coder] planner: plan already exists, skipping" << std::endl;
        this->push_event_(AgentEvent::Status,
            "План уже существует — продолжаю по нему.");
        return true;
    }
    std::cerr << "[wp_coder] planner: no existing plan, generating..." << std::endl;

    std::vector<ChatMsg> plan_msgs;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        plan_msgs = state_.session;
        plan_msgs.push_back({"user",
            "Составь краткий пошаговый план решения задачи (без вызова "
            "инструментов, без wp_action). Перечисли только шагам одним "
            "нумерованным списком. Потом ты выполнишь их инструментами."});
    }
    this->push_event_(AgentEvent::Status, "Составляю план...");
    LlmReply plan_reply;
    bool plan_ok = false;
    if (cb_.llm_chat) {
        plan_ok = cb_.llm_chat(sys_prompt, plan_msgs, plan_reply);
    } else if (cb_.llm_complete) {
        std::string last_user;
        for (const auto& m : plan_msgs)
            if (m.role == "user") last_user = m.content;
        std::string resp;
        plan_ok = cb_.llm_complete(sys_prompt, last_user, resp);
        if (plan_ok) {
            plan_reply.content = resp;
            plan_reply.prompt_tokens = 0;
            plan_reply.completion_tokens = 0;
        }
    }
    if (plan_ok && !plan_reply.content.empty() &&
        !state_.abort_requested.load()) {
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            state_.session.push_back({"assistant",
                "[ПЛАН]\n" + plan_reply.content});
            state_.total_prompt_tokens += plan_reply.prompt_tokens;
            state_.total_completion_tokens += plan_reply.completion_tokens;
        }
        this->push_event_(AgentEvent::Assistant,
            "План: " + plan_reply.content);
        return true;
    } else if (!plan_ok && !state_.abort_requested.load()) {
        this->push_event_(AgentEvent::Error, "Не удалось составить план — работаю без него.");
    }
    return false;
}

/* ======================================================================
 * AgentLoop
 * ====================================================================== */

bool AgentLoop::run(const std::string& sys_prompt, std::string& full_response) {
    bool final_given = false;
    int stuck_counter = 0;  // последовательных коротких ответов

    std::cerr << "[wp_coder] agent_loop: starting, max_steps=" << state_.max_steps << std::endl;

    for (int step = 0; step < state_.max_steps; ++step) {
        if (state_.abort_requested.load()) {
            this->push_event_(AgentEvent::Status, "[прервано пользователем]");
            full_response += "\n\n[прервано пользователем]";
            {
                std::lock_guard<std::mutex> lk(state_.mtx);
                state_.session.clear();
            }
            return true;
        }

        std::vector<ChatMsg> msgs;
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            msgs = state_.session;
        }

        LlmReply reply;
        bool ok = false;
        auto t0 = std::chrono::steady_clock::now();

        /* Диагностика: размер контекста, отправляемого в LLM. */
        {
            size_t total_chars = 0;
            for (const auto& m : msgs) total_chars += m.content.size();
            std::cout << "[wp_coder] step " << (step + 1) << ": msgs=" << msgs.size()
                      << " chars=" << total_chars << std::endl;
        }

        std::cerr << "[wp_coder] agent_loop: step " << (step+1) << " calling LLM..." << std::endl;
        if (cb_.llm_chat) {
            ok = cb_.llm_chat(sys_prompt, msgs, reply);
        } else if (cb_.llm_complete) {
            std::string last_user;
            for (const auto& m : msgs)
                if (m.role == "user") last_user = m.content;
            std::string resp;
            ok = cb_.llm_complete(sys_prompt, last_user, resp);
            if (ok) {
                reply.content = resp;
                reply.finish_reason = "stop";
                reply.prompt_tokens = 0;
                reply.completion_tokens = 0;
            }
        }
        auto t1 = std::chrono::steady_clock::now();
        double llm_s = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() / 1000.0;

        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            state_.steps = step + 1;
            state_.llm_total_time += llm_s;
            state_.total_prompt_tokens += reply.prompt_tokens;
            state_.total_completion_tokens += reply.completion_tokens;
        }

        /* A4: usage токенов шага. */
        {
            std::string usage_line = "step " + std::to_string(step + 1) + ": "
                + std::to_string(reply.prompt_tokens) + "+"
                + std::to_string(reply.completion_tokens) + " tok, "
                + std::to_string((int)llm_s) + "s";
            this->push_event_(AgentEvent::Status, usage_line);
        }

        if (!ok || reply.content.empty()) {
            std::string err = reply.error.empty() ? "не ответил" : reply.error;
            std::string diag;
            {
                std::lock_guard<std::mutex> lk(state_.mtx);
                diag = "session_msgs=" + std::to_string(state_.session.size())
                     + " step=" + std::to_string(step + 1);
            }
            this->push_event_(AgentEvent::Error,
                (std::string("[ошибка] LLM: ") + err + " (" + diag + ")").c_str());
            full_response = full_response.empty()
                ? "[ошибка] LLM: " + err : full_response;
            return false;
        }

        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            state_.session.push_back({"assistant", reply.content});
        }

        /* Извлекаем вызов инструмента ДО stuck-детектора: если модель
         * вызвала инструмент — она прогрессирует, независимо от длины ответа.
         * Раньше короткие tool call-ы (< 200 символов) ошибочно считались
         * признаком застревания, и после 3 таких вызовов агент прерывался
         * с ошибкой "LLM: не ответил". */
        std::string rest;
        std::string block = extract_action(reply.content, rest);

        /* Детектор застревания: учитываем ТОЛЬКО короткие текстовые ответы
         * БЕЗ вызова инструмента. Tool call = прогресс → сброс счётчика. */
        if (reply.content.size() < kShortResponseLen && block.empty()) {
            ++stuck_counter;
            if (stuck_counter >= kStuckThreshold) {
                this->push_event_(AgentEvent::Status,
                    "Модель застряла — запрашиваю итоговый ответ.");
                {
                    std::lock_guard<std::mutex> lk(state_.mtx);
                    state_.session.push_back({"user",
                        "Ты делаешь очень короткие ответы и не прогрессируешь. "
                        "Инструменты больше вызывать НЕЛЬЗЯ. "
                        "Дай итоговый ответ по результатам проделанной работы."});
                }
                std::vector<ChatMsg> final_msgs;
                {
                    std::lock_guard<std::mutex> lk(state_.mtx);
                    final_msgs = state_.session;
                }
                LlmReply final_reply;
                bool f_ok = false;
                if (cb_.llm_chat)
                    f_ok = cb_.llm_chat(sys_prompt, final_msgs, final_reply);
                if (f_ok && !final_reply.content.empty()) {
                    std::string f_rest;
                    extract_action(final_reply.content, f_rest);
                    std::string f_text = f_rest.empty() ? final_reply.content : f_rest;
                    if (!f_text.empty()) {
                        if (!full_response.empty()) full_response += "\n\n";
                        full_response += f_text;
                        this->push_event_(AgentEvent::Assistant, f_text);
                    }
                }
                return true;
            }
        } else {
            stuck_counter = 0;  // tool call или длинный ответ = прогресс
        }

        if (reply.finish_reason == "length") {
            this->push_event_(AgentEvent::Status,
                "Внимание: модель упёрлась в лимит токенов (finish=length). "
                "Разбей задачу на шаги.");

            SessionStore trimmer(state_);
            trimmer.trim();
        }

        /* Автосжатие: даже без finish=length, если сессия превысила бюджет,
         * сжимаем старые RESULT — иначе модель теряет контекст. */
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            size_t total = 0;
            for (const auto& m : state_.session) total += m.content.size();
            if (total > state_.session_budget) {
                SessionStore trimmer(state_);
                trimmer.trim();
            }
        }
        if (!rest.empty()) {
            if (!full_response.empty()) full_response += "\n\n";
            full_response += rest;
            this->push_event_(AgentEvent::Assistant, rest);

            if (!block.empty()) {
                {
                    std::lock_guard<std::mutex> lk(state_.mtx);
                    auto& s = state_.session;
                    if (!s.empty() && s.back().role == "assistant") {
                        std::string condensed = block;
                        if (condensed.size() > 1200) { condensed.resize(1200); condensed += "…"; }
                        s.back().content = condensed;
                    }
                }
            }
        }

        if (block.empty()) {
            final_given = true;
            this->push_event_(AgentEvent::Status, "Готово (финальный ответ).");
            return true;
        }

        Action act;
        if (!parse_action(block, act)) {
            this->push_event_(AgentEvent::Error, (std::string("[ошибка разбора wp_action] блок:\n") + block).c_str());
            full_response += "\n\n[ошибка разбора wp_action]";
            return false;
        }

        ToolArgs args;
        args.path = act.path;
        args.root = act.root;
        args.query = act.query;
        args.pattern = act.pattern;
        args.content = act.content;
        args.cli = act.cli;
        args.url = act.url;
        args.k = act.k;

        ToolRunner tool_runner(state_, cb_, this->push_event_);
        std::string result = tool_runner.run(act.tool, args);

        std::string perm_path;
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            if (state_.state == AgentState::WaitingPermission)
                perm_path = state_.pending_permission_path;
        }

        if (!perm_path.empty()) {
            {
                std::lock_guard<std::mutex> lk(state_.mtx);
                std::string trimmed_result = result;
                if (trimmed_result.size() > kResultBudget) {
                    trimmed_result.resize(kResultBudget);
                    trimmed_result += "\n[...обрезано, всего " + std::to_string(result.size())
                                    + " символов. Вызови инструмент повторно, если нужно больше.]";
                }
                state_.session.push_back({"user", "RESULT [" + act.tool + "]:\n" + trimmed_result});
                state_.waiting_in_sync = true;
            }
            this->push_event_(AgentEvent::Status, "Ожидание разрешения: " + perm_path);
            if (cb_.chat_event)
                cb_.chat_event("[ai-coder] ⏸ ожидание разрешения: " + perm_path);

            {
                std::unique_lock<std::mutex> lk(state_.mtx);
                state_.permission_cv.wait(lk, [this] {
                    return state_.state != AgentState::WaitingPermission
                        || state_.shutting_down || state_.abort_requested.load();
                });
                state_.waiting_in_sync = false;
            }

            if (state_.shutting_down) {
                full_response += "\n\n[агент остановлен]";
                return false;
            }
            if (state_.abort_requested.load()) {
                full_response += "\n\n[прервано пользователем]";
                {
                    std::lock_guard<std::mutex> lk(state_.mtx);
                    state_.session.clear();
                }
                return true;
            }

            this->push_event_(AgentEvent::Status, "Доступ разрешён — continuaю.");
            {
                std::lock_guard<std::mutex> lk(state_.mtx);
                state_.session.push_back({"user",
                    "(пользователь разрешил доступ к " + perm_path
                    + "; разрешение действует. Выполни инструмент ещё раз "
                    "с теми же аргументами.)"});
                state_.allowed_external_paths.push_back(perm_path);
            }
            continue;
        }

        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            if (state_.abort_requested.load()) {
                full_response += "\n\n[прервано пользователем]";
                state_.session.clear();
                return true;
            }
            /* Обрезаем RESULT до kResultBudget символов — это главный способ
             * экономии токенов. Полный результат может быть 5-10К+ символов,
             * и он отправляется на КАЖДОМ следующем шаге. Модели достаточно
             * начала для принятия решения; если нужно больше — вызовет repeat. */
            std::string trimmed_result = result;
            if (trimmed_result.size() > kResultBudget) {
                trimmed_result.resize(kResultBudget);
                trimmed_result += "\n[...обрезано, всего " + std::to_string(result.size())
                                + " символов. Вызови инструмент повторно, если нужно больше.]";
            }
            state_.session.push_back({"user", "RESULT [" + act.tool + "]:\n" + trimmed_result});
        }
    }

    if (!final_given && !state_.abort_requested.load()) {
        this->push_event_(AgentEvent::Status,
            "Лимит шагов исчерпан — прошу итоговый ответ.");
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            state_.session.push_back({"user",
                "Лимит шагов ReAct исчерпан. Инструменты больше вызывать НЕЛЬЗЯ. "
                "Дай пользователю итоговый ответ по результатам проделанной работы."});
        }
        std::vector<ChatMsg> msgs;
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            msgs = state_.session;
        }
        LlmReply reply;
        bool ok = false;
        if (cb_.llm_chat) {
            ok = cb_.llm_chat(sys_prompt, msgs, reply);
        } else if (cb_.llm_complete) {
            std::string last_user;
            for (const auto& m : msgs)
                if (m.role == "user") last_user = m.content;
            std::string resp;
            ok = cb_.llm_complete(sys_prompt, last_user, resp);
            if (ok) {
                reply.content = resp;
                reply.finish_reason = "stop";
                reply.prompt_tokens = 0;
                reply.completion_tokens = 0;
            }
        }
        if (ok && !reply.content.empty()) {
            std::string rest;
            std::string block = extract_action(reply.content, rest);
            std::string final_text = rest.empty() ? reply.content : rest;
            if (!final_text.empty()) {
                if (!full_response.empty()) full_response += "\n\n";
                full_response += final_text;
                this->push_event_(AgentEvent::Assistant, final_text);
            }
            {
                std::lock_guard<std::mutex> lk(state_.mtx);
                state_.steps += 1;
                state_.total_prompt_tokens += reply.prompt_tokens;
                state_.total_completion_tokens += reply.completion_tokens;
            }
        }
    }

    return final_given;
}

/* ======================================================================
 * main
 * ====================================================================== */

} // namespace coder
