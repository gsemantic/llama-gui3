// engine.cpp — AI-кодер с разбиением run_task на компоненты (Фаза D1)

#include "agent_components.h"
#include "prompts.h"
#include "shell.h"
#include "engine.h"
#include "limits.h"
#include "json_utils.h"

#include <sstream>
#include <vector>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <fstream>
#include <iterator>
#include <filesystem>

namespace coder {

/* Бюджет символов на историю сессии — единый источник: core/limits.h (4.5). */
using limits::kSessionBudget;

/*
 * Engine — глобальный singleton
 * ====================================================================== */

Engine& Engine::instance() {
    static Engine eng;
    return eng;
}

void Engine::init(HostCallbacks callbacks) {
    cb_ = std::move(callbacks);
    load_settings();
}

void Engine::start() {
    state_.worker = std::thread(&Engine::worker_main, this);
}

void Engine::stop() {
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.shutting_down = true;
        state_.permission_cv.notify_all();
        state_.cv.notify_all();
    }
    if (state_.worker.joinable()) state_.worker.join();
    /* Resume (5.2): сохраняем последний диалог перед выключением. */
    save_session();
}

void Engine::submit(const std::string& prompt) {
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        bool resume = state_.preserve_session;
        state_.preserve_session = false;
        if (resume) {
            /* Возобновление после паузы на разрешение: не модифицируем сессию. */
        } else if (state_.continue_conversation && !state_.session.empty()) {
            /* Продолжение сессии: добавляем сообщение к существующему контексту.
             * План и предыдущие результаты сохраняются — модель знает историю. */
            state_.session.push_back({"user", prompt});
            std::cerr << "[wp_coder] submit: continue session, msgs="
                      << state_.session.size() << std::endl;
        } else {
            /* Новая задача: очищаем сессию и начинаем заново. */
            state_.session.clear();
            state_.session.push_back({"user", prompt});
            std::cerr << "[wp_coder] submit: new session" << std::endl;
        }
        state_.last_agent_task = prompt;
        state_.response_ready = false;
        state_.last_response.clear();
        state_.abort_requested.store(false);
        state_.inbox.push(prompt);
        state_.cv.notify_all();
        std::cerr << "[wp_coder] submit: inbox_size=" << state_.inbox.size()
                  << " state=" << agent_state_name(state_.state) << std::endl;
    }
}

void Engine::request_abort() {
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.abort_requested.store(true);
        /* Не переводим FSM здесь: wait()-предикаты включают abort_requested,
         * поток разбудится и AgentLoop/cleanup сам переведёт в Aborted. */
        state_.permission_cv.notify_all();
    }
}

void Engine::clear_session() {
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.session.clear();
        state_.recent_calls.clear();
        state_.last_agent_task.clear();
    }  /* mtx отпущен — push_event безопасен */
    /* Удаляем и сохранённую на диске сессию (resume, 5.2). */
    std::string path = session_file_path();
    if (!path.empty()) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    push_event(AgentEvent::Status, "Сессия очищена — следующий запрос начнётся заново.");
}

/* Resume сессии (5.2). */
std::string Engine::session_file_path() const {
    if (!cb_.path_data_dir) return "";
    std::string dir = cb_.path_data_dir();
    if (dir.empty()) return "";
    return dir + "/wp_coder/session.json";
}

void Engine::save_session() {
    std::string path = session_file_path();
    if (path.empty()) return;
    /* Каталог <data_dir>/wp_coder может не существовать — создаём. */
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::vector<ChatMsg> snap;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        snap = state_.session;
    }
    std::string json = "[";
    for (size_t i = 0; i < snap.size(); ++i) {
        if (i) json += ",";
        json += "{\"role\":\"" + snap[i].role + "\",\"content\":\""
              + json::escape(snap[i].content) + "\"}";
    }
    json += "]";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f) f << json;
}

void Engine::load_session() {
    std::string path = session_file_path();
    if (path.empty()) return;
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());

    std::vector<ChatMsg> loaded;
    size_t pos = 0;
    while ((pos = content.find("{\"role\"", pos)) != std::string::npos) {
        size_t end = content.find('}', pos);
        if (end == std::string::npos) break;
        std::string item = content.substr(pos, end - pos + 1);
        ChatMsg m;
        m.role = json::str(item, "role");
        m.content = json::str(item, "content");
        if (!m.role.empty()) loaded.push_back(std::move(m));
        pos = end + 1;
    }
    if (loaded.empty()) return;

    std::lock_guard<std::mutex> lk(state_.mtx);
    if (state_.session.empty()) state_.session = std::move(loaded);
}

std::string Engine::wait_response(int timeout_ms) {
    std::unique_lock<std::mutex> lk(state_.mtx);
    if (timeout_ms > 0) {
        state_.response_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [this] {
            return state_.response_ready || state_.shutting_down;
        });
    } else {
        state_.response_cv.wait(lk, [this] {
            return state_.response_ready || state_.shutting_down;
        });
    }
    if (state_.response_ready) {
        state_.response_ready = false;
        return std::move(state_.last_response);
    }
    request_abort();
    return "[ошибка] таймаут ожидания ответа агента";
}

/* ======================================================================
 * Сборка системного промпта
 * ====================================================================== */

std::string Engine::build_system_prompt() const {
    if (!state_.prompt_dirty && !state_.cached_system_prompt.empty())
        return state_.cached_system_prompt;

    /* project_dir — ПЕРВЫМ, чтобы модель точно увидела корень проекта.
     * Даже при длинном кастомном промпте эта информация не потеряется. */
    std::string sys;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        if (!state_.project_dir.empty()) {
            sys = "## КОРНЕВОЙ КАТАЛОГ ПРОЕКТА\n"
                  "Корень: " + state_.project_dir + "\n"
                  "Все пути в инструментах — относительно этого каталога.\n\n";
        }
    }

    sys += state_.agent_system_prompt.empty()
        ? std::string(kBaseSystemPrompt)
        : state_.agent_system_prompt;

    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        if (!state_.active_module.empty()) {
            const auto& modules = ModuleRegistry::instance().modules();
            for (const auto* mod : modules) {
                if (state_.active_module == mod->name && mod->get_system_prompt) {
                    const char* p = mod->get_system_prompt();
                    if (p && p[0]) {
                        sys += "\n\n";
                        sys += p;
                    }
                    break;
                }
            }
        }
    }

    sys += SkillsManager::instance().build_skills_prompt();

    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        if (state_.mode == 1) sys += kModeResearch;
        else if (state_.mode == 2) sys += kModeReview;
    }

    state_.cached_system_prompt = sys;
    state_.prompt_dirty = false;
    return sys;
}

/* ======================================================================
 * Утилиты для Engine (обертки над component классами)
 * ====================================================================== */

void Engine::pending_apply(size_t idx) {
    std::lock_guard<std::mutex> lk(state_.mtx);
    if (idx >= state_.pending.size()) return;
    const auto& p = state_.pending[idx];
    std::string abs = p.path;
    if (!abs.empty() && abs[0] != '/' && !state_.project_dir.empty()) {
        abs = state_.project_dir + "/" + abs;
    }
    std::ofstream f(abs, std::ios::binary);
    if (f) { f << p.content; f.close(); }
    state_.pending.erase(state_.pending.begin() + idx);
}

void Engine::pending_discard(size_t idx) {
    std::lock_guard<std::mutex> lk(state_.mtx);
    if (idx >= state_.pending.size()) return;
    state_.pending.erase(state_.pending.begin() + idx);
}

std::string Engine::check_external_permission(const std::string& abs_path) {
    auto push = [this](AgentEvent::Kind k, const std::string& t) { push_event(k, t); };
    PermissionGate gate(state_, cb_, push);
    return gate.check(abs_path);
}

void Engine::permission_allow_once(const std::string& path) {
    auto push = [this](AgentEvent::Kind k, const std::string& t) { push_event(k, t); };
    PermissionGate gate(state_, cb_, push);
    gate.allow_once(path);
}

void Engine::permission_allow_always(const std::string& path) {
    auto push = [this](AgentEvent::Kind k, const std::string& t) { push_event(k, t); };
    PermissionGate gate(state_, cb_, push);
    gate.allow_always(path);
}

void Engine::permission_reject(const std::string& path) {
    auto push = [this](AgentEvent::Kind k, const std::string& t) { push_event(k, t); };
    PermissionGate gate(state_, cb_, push);
    gate.reject();
}

/* FSM (2.3): переход состояния. Observer-событие публикуется только при
 * реальном изменении — не спамим лог повторными переходами. */
void Engine::set_state(AgentState s) {
    AgentState prev;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        prev = state_.state;
        state_.state = s;
    }
    if (prev != s)
        push_event(AgentEvent::Status,
            std::string("state: ") + agent_state_name(s));
    else
        state_.permission_cv.notify_all();
}

/* ======================================================================
 * События
 * ====================================================================== */

void Engine::push_event(AgentEvent::Kind k, const std::string& text) {
    bool forward = false;
    std::string chat_line;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.events.push_back({k, text});
        if (state_.events.size() > 500) state_.events.pop_front();
    }
    if (cb_.chat_event) {
        switch (k) {
            case AgentEvent::Status:
                chat_line = text;
                forward = true;
                break;
            case AgentEvent::Error:
                chat_line = "! " + text;
                forward = true;
                break;
            case AgentEvent::Assistant:
                chat_line = text;
                forward = true;
                break;
            case AgentEvent::Tool:
                chat_line = "🛠 " + text;
                forward = true;
                break;
            default:
                break;
        }
        if (forward && !chat_line.empty()) {
            if (chat_line.size() > 300) { chat_line.resize(300); chat_line += "…"; }
            cb_.chat_event(chat_line);
        }
    }
}

/* ======================================================================
 * Настройки (загрузка/сохранение)
 * ====================================================================== */

static std::string setting_get(const HostCallbacks& cb, const std::string& key, const std::string& def) {
    if (!cb.settings_get) return def;
    std::string v = cb.settings_get(key, def);
    if (v.empty()) return def;
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
        v = v.substr(1, v.size() - 2);
    return v;
}

static void setting_set(const HostCallbacks& cb, const std::string& key, const std::string& value) {
    if (!cb.settings_set) return;
    cb.settings_set(key, "\"" + value + "\"");
}

void Engine::load_settings() {
    state_.project_dir      = setting_get(cb_, "wp_coder.project_dir", "");
    if (state_.project_dir.empty()) {
        std::cout << "[wp_coder] ВНИМАНИЕ: корневой каталог проекта не задан — "
                  << "агент не будет знать, где находится код" << std::endl;
    }
    state_.php_bin          = setting_get(cb_, "wp_coder.php_bin", "");
    state_.wp_site_url      = setting_get(cb_, "wp_coder.site_url", "");
    state_.wp_app_user      = setting_get(cb_, "wp_coder.app_user", "");
    state_.wp_app_password  = setting_get(cb_, "wp_coder.app_password", "");
    state_.deploy_proto     = setting_get(cb_, "wp_coder.deploy_proto", "rsync");
    state_.deploy_host      = setting_get(cb_, "wp_coder.deploy_host", "");
    state_.deploy_user      = setting_get(cb_, "wp_coder.deploy_user", "");
    state_.deploy_pass      = setting_get(cb_, "wp_coder.deploy_pass", "");
    state_.deploy_port      = setting_get(cb_, "wp_coder.deploy_port", "");
    state_.deploy_remote_dir = setting_get(cb_, "wp_coder.deploy_remote_dir", "");
    state_.wp_local_url    = setting_get(cb_, "wp_coder.local_url", "");
    state_.agent_system_prompt = setting_get(cb_, "wp_coder.agent_system_prompt", "");
    state_.active_module    = setting_get(cb_, "wp_coder.active_module", "");
    state_.continue_conversation = setting_get(cb_, "wp_coder.continue_conversation", "true") != "false";
    {
        int timeout = 120000;
        std::string t = setting_get(cb_, "wp_coder.llm_timeout_ms", "120000");
        try { timeout = std::stoi(t); } catch (...) {}
        state_.llm_timeout_ms = timeout;
    }
    {
        int steps = 12;
        std::string t = setting_get(cb_, "wp_coder.max_steps", "12");
        try { steps = std::stoi(t); } catch (...) {}
        state_.max_steps = steps > 0 ? steps : 12;
    }
    {
        int budget = 60000;
        std::string t = setting_get(cb_, "wp_coder.session_budget", "60000");
        try { budget = std::stoi(t); } catch (...) {}
        state_.session_budget = budget > 0 ? static_cast<size_t>(budget) : 60000;
    }

    state_.allowed_external_paths.clear();
    std::string paths_json = setting_get(cb_, "wp_coder.allowed_external_paths", "[]");
    {
        size_t pos = 0;
        while (pos < paths_json.size()) {
            size_t q1 = paths_json.find('"', pos);
            if (q1 == std::string::npos) break;
            size_t q2 = paths_json.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            state_.allowed_external_paths.push_back(paths_json.substr(q1 + 1, q2 - q1 - 1));
            pos = q2 + 1;
        }
    }
}

void Engine::save_settings() {
    setting_set(cb_, "wp_coder.project_dir", state_.project_dir);
    setting_set(cb_, "wp_coder.php_bin", state_.php_bin);
    setting_set(cb_, "wp_coder.site_url", state_.wp_site_url);
    setting_set(cb_, "wp_coder.app_user", state_.wp_app_user);
    setting_set(cb_, "wp_coder.app_password", state_.wp_app_password);
    setting_set(cb_, "wp_coder.deploy_proto", state_.deploy_proto);
    setting_set(cb_, "wp_coder.deploy_host", state_.deploy_host);
    setting_set(cb_, "wp_coder.deploy_user", state_.deploy_user);
    setting_set(cb_, "wp_coder.deploy_pass", state_.deploy_pass);
    setting_set(cb_, "wp_coder.deploy_port", state_.deploy_port);
    setting_set(cb_, "wp_coder.deploy_remote_dir", state_.deploy_remote_dir);
    setting_set(cb_, "wp_coder.local_url", state_.wp_local_url);
    setting_set(cb_, "wp_coder.agent_system_prompt", state_.agent_system_prompt);
    setting_set(cb_, "wp_coder.active_module", state_.active_module);
    setting_set(cb_, "wp_coder.continue_conversation", state_.continue_conversation ? "true" : "false");
    setting_set(cb_, "wp_coder.llm_timeout_ms", std::to_string(state_.llm_timeout_ms));
    setting_set(cb_, "wp_coder.max_steps", std::to_string(state_.max_steps));
    setting_set(cb_, "wp_coder.session_budget", std::to_string(state_.session_budget));
}

/* ======================================================================
 * run_task — разбит на компоненты (Фаза D1)
 * goto устранен: cleanup вынесен в lambda.
 * ====================================================================== */

void Engine::run_task(const std::string& task) {
    auto start_time = std::chrono::steady_clock::now();
    std::string full_response;

    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.state = AgentState::Planning;
        state_.last_response.clear();
        state_.response_ready = false;
        state_.abort_requested.store(false);
        state_.llm_total_time = 0;
        state_.total_prompt_tokens = 0;
        state_.total_completion_tokens = 0;
        state_.last_tokens_per_second = 0;
        state_.steps = 0;
        if (state_.session.empty())
            state_.session.push_back({"user", task});
        std::cerr << "[wp_coder] run_task: session_msgs=" << state_.session.size()
                  << " continue=" << state_.continue_conversation << std::endl;
    }
    push_event(AgentEvent::Status, "Задача: " + task);

    /* Lambda-очистка — аналог блока done:, вызывается всегда. */
    auto cleanup = [&]() {
        {
            std::lock_guard<std::mutex> lk(state_.mtx);

            /* allow_once: убираем одноразовое разрешение после задачи. */
            if (!state_.once_path.empty()) {
                auto& v = state_.allowed_external_paths;
                auto it = std::find(v.begin(), v.end(), state_.once_path);
                if (it != v.end()) v.erase(it);
                state_.once_path.clear();
            }

            /* FSM: итоговое состояние — прервано пользователем или завершено. */
            state_.state = state_.abort_requested.load()
                ? AgentState::Aborted : AgentState::Done;

            state_.last_response = full_response.empty() ? "(пустой ответ)" : full_response;

            auto end_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            state_.last_response_time = duration.count() / 1000.0;

            int comp_tokens = state_.total_completion_tokens;
            double llm_t = state_.llm_total_time;
            if (comp_tokens <= 0 && !state_.last_response.empty())
                comp_tokens = std::max(1, static_cast<int>(state_.last_response.size() / 4));
            state_.last_tokens_generated = comp_tokens;
            state_.last_tokens_per_second = (llm_t > 0.0) ? (double)comp_tokens / llm_t : 0.0;

            state_.response_ready = true;
        }
        state_.response_cv.notify_all();
        /* Resume (5.2): сохраняем диалог после каждой задачи —
         * при следующем запуске можно продолжить. */
        save_session();
    };

    if (!cb_.llm_is_connected || !cb_.llm_is_connected()) {
        push_event(AgentEvent::Error, "[ошибка] LLM не подключён");
        full_response = "[ошибка] LLM не подключён";
        cleanup();
        return;
    }
    std::cerr << "[wp_coder] run_task: LLM connected, starting plan+loop" << std::endl;

    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.last_agent_task = task;
    }

    {
        std::string sys = build_system_prompt();
        std::cerr << "[wp_coder] run_task: system prompt built, len="
                  << sys.size() << std::endl;

        /* D1: Фаза B1 — планирование (если включен). */
        Planner planner(state_, cb_, [this](AgentEvent::Kind k, const std::string& text) {
            push_event(k, text);
        });
        std::cerr << "[wp_coder] run_task: calling planner.plan()..." << std::endl;
        planner.plan(sys);
        std::cerr << "[wp_coder] run_task: planner.plan() done" << std::endl;

        /* FSM: план готов (или пропущен) — переходим к исполнению. */
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            state_.state = AgentState::Executing;
        }

        /* D1: Фаза C — основной ReAct-цикл через AgentLoop. */
        AgentLoop agent_loop(state_, cb_, [this](AgentEvent::Kind k, const std::string& text) {
            push_event(k, text);
        });
        std::cerr << "[wp_coder] run_task: calling agent_loop.run()..." << std::endl;
        agent_loop.run(sys, full_response);
        std::cerr << "[wp_coder] run_task: agent_loop.run() done" << std::endl;
    }

    cleanup();
}

/* ======================================================================
 * worker_main
 * ====================================================================== */

void Engine::worker_main() {
    for (;;) {
        std::string task;
        {
            std::unique_lock<std::mutex> lk(state_.mtx);
            state_.cv.wait(lk, [this] {
                return state_.shutting_down || !state_.inbox.empty();
            });
            if (state_.shutting_down) break;
            task = std::move(state_.inbox.front());
            state_.inbox.pop();
        }
        std::cerr << "[wp_coder] worker: picked up task, len=" << task.size() << std::endl;
        run_task(task);
        std::cerr << "[wp_coder] worker: run_task done" << std::endl;
    }
}

/* ======================================================================
 * Тестовый метод trim_session_test (для unit-тестов).
 * ====================================================================== */

void Engine::trim_session_test() {
    SessionStore(state_).trim();
}

/* ======================================================================
 * Парсинг вызова инструмента перенесён в core/tool_protocol.{h,cpp} (Фаза D2).
 * В Engine остались тонкие алиасы Engine::extract_action / Engine::parse_action.
 * ====================================================================== */

/* ======================================================================
 * Предложенные правки
 * ====================================================================== */

} // namespace coder
