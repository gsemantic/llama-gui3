#include "engine.h"
#include "prompts.h"

#include <sstream>
#include <vector>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;
namespace coder {

/* ======================================================================
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
        state_.cv.notify_all();
    }
    if (state_.worker.joinable()) state_.worker.join();
}

void Engine::submit(const std::string& prompt) {
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.inbox.push(prompt);
        state_.response_ready = false;
        state_.last_response.clear();
        state_.cv.notify_all();
    }
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
    return "[ошибка] таймаут ожидания ответа агента";
}

/* ======================================================================
 * Сборка системного промпта
 * ====================================================================== */

std::string Engine::build_system_prompt() const {
    /* 1. Базовый промпт. */
    std::string sys = state_.agent_system_prompt.empty()
        ? std::string(kBaseSystemPrompt)
        : state_.agent_system_prompt;

    /* 2. Корень проекта. */
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        if (!state_.project_dir.empty()) {
            sys += "\n\n## КОРНЕВОЙ КАТАЛОГ ПРОЕКТА\n"
                   "Корень: " + state_.project_dir + "\n"
                   "Все пути в инструментах — относительно этого каталога.\n"
                   "Если пользователь указывает относительный путь — "
                   "он разрешается относительно корня автоматически.";
        }
    }

    /* 3. Промпты активного модуля. */
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        const auto& modules = ModuleRegistry::instance().modules();
        for (const auto* mod : modules) {
            /* Если задан active_module — только он. Иначе — все. */
            if (!state_.active_module.empty() && state_.active_module != mod->name)
                continue;
            if (mod->get_system_prompt) {
                const char* p = mod->get_system_prompt();
                if (p && p[0]) {
                    sys += "\n\n";
                    sys += p;
                }
            }
        }
    }

    /* 4. Навыки. */
    sys += SkillsManager::instance().build_skills_prompt();

    /* 5. Режим. */
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        if (state_.mode == 1) sys += kModeResearch;
        else if (state_.mode == 2) sys += kModeReview;
    }

    return sys;
}

/* ======================================================================
 * ReAct-цикл
 * ====================================================================== */

void Engine::run_task(const std::string& task) {
    std::string full_response;
    auto start_time = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.running = true;
        state_.last_response.clear();
        state_.response_ready = false;
    }
    push_event(AgentEvent::Status, "Задача: " + task);

    if (!cb_.llm_is_connected || !cb_.llm_is_connected()) {
        push_event(AgentEvent::Error, "[ошибка] LLM не подключён");
        full_response = "[ошибка] LLM не подключён";
        goto done;
    }

    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.last_agent_task = task;
    }

    {
        std::string user = task;
        for (int step = 0; step < 12; ++step) {
            std::string sys = build_system_prompt();

            std::string resp;
            if (!cb_.llm_complete || !cb_.llm_complete(sys, user, resp)) {
                push_event(AgentEvent::Error, "[ошибка] LLM не ответил");
                full_response = "[ошибка] LLM не ответил";
                break;
            }

            std::string rest;
            std::string block = extract_action(resp, rest);
            if (!rest.empty()) {
                if (!full_response.empty()) full_response += "\n\n";
                full_response += rest;
                push_event(AgentEvent::Assistant, rest);
            }

            if (block.empty()) {
                push_event(AgentEvent::Status, "Готово (финальный ответ).");
                break;
            }

            Action act;
            if (!parse_action(block, act)) {
                push_event(AgentEvent::Error, "[ошибка разбора wp_action] блок:\n" + block);
                full_response += "\n\n[ошибка разбора wp_action]";
                break;
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

            std::string result = ToolsRegistry::instance().run(act.tool, args);
            push_event(AgentEvent::Tool, act.tool + " -> " + result);

            /* Если tool_run запросил разрешение — сигналим и ждём решение. */
            {
                std::lock_guard<std::mutex> lk(state_.mtx);
                if (state_.waiting_for_permission) {
                    push_event(AgentEvent::Status, "Ожидание разрешения...");
                    if (!state_.waiting_for_permission)
                        state_.running = false;
                    state_.last_response = full_response.empty()
                        ? "Ожидание разрешения доступа..."
                        : full_response;
                    state_.response_ready = true;
                    state_.response_cv.notify_all();
                    return;
                }
            }

            user = "RESULT [" + act.tool + "]:\n" + result;
        }
    }

    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        if (!state_.once_path.empty()) {
            auto& v = state_.allowed_external_paths;
            for (auto it = v.begin(); it != v.end(); ) {
                if (*it == state_.once_path) it = v.erase(it);
                else ++it;
            }
            state_.once_path.clear();
        }
    }

done:
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        if (!state_.waiting_for_permission)
            state_.running = false;
        state_.last_response = full_response.empty() ? "(пустой ответ)" : full_response;
        /* Метрики. */
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        state_.last_response_time = duration.count() / 1000.0;
        state_.last_tokens_generated = std::max(1, (int)(full_response.size() / 4));
        state_.last_tokens_per_second = state_.last_response_time > 0
            ? state_.last_tokens_generated / state_.last_response_time : 0;
        state_.response_ready = true;
        state_.response_cv.notify_all();
    }
}

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
        run_task(task);
    }
}

/* ======================================================================
 * Парсинг wp_action
 * ====================================================================== */

std::string Engine::extract_action(const std::string& text, std::string& rest) {
    rest = text;
    size_t a = text.find("```wp_action");
    if (a == std::string::npos) {
        a = text.find("```\nwp_action");
        if (a == std::string::npos) {
            a = text.find("```");
            if (a == std::string::npos) return "";
            size_t body_check = text.find('\n', a);
            if (body_check == std::string::npos) return "";
            size_t wp = text.find("wp_action", body_check + 1);
            size_t close = text.find("```", body_check + 1);
            if (wp == std::string::npos || (close != std::string::npos && wp > close)) return "";
        }
    }
    size_t body = text.find('\n', a);
    if (body == std::string::npos) return "";
    size_t b = text.find("```", body);
    if (b == std::string::npos) return "";
    std::string block = text.substr(body + 1, b - body - 1);
    rest = text.substr(0, a) + text.substr(b + 3);
    return block;
}

bool Engine::parse_action(const std::string& block, Action& a) {
    std::istringstream iss(block);
    std::string line;
    bool in_content = false;
    while (std::getline(iss, line)) {
        /* trim trailing \r */
        while (!line.empty() && line.back() == '\r') line.pop_back();

        if (in_content) {
            if (line.find("CONTENT_END") != std::string::npos) { in_content = false; continue; }
            if (!a.content.empty()) a.content += '\n';
            a.content += line;
            continue;
        }
        /* Проверяем CONTENT_BEGIN без двоеточия. */
        if (line.find("CONTENT_BEGIN") != std::string::npos) { in_content = true; continue; }
        auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);
        auto trim = [](std::string& s) {
            while (!s.empty() && s.back() == '\r') s.pop_back();
            size_t b = s.find_first_not_of(" \t");
            size_t e = s.find_last_not_of(" \t");
            s = (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
        };
        trim(key); trim(val);
        if (key == "TOOL") a.tool = val;
        else if (key == "PATH") a.path = val;
        else if (key == "ROOT") a.root = val;
        else if (key == "QUERY") a.query = val;
        else if (key == "PATTERN") a.pattern = val;
        else if (key == "CLI") a.cli = val;
        else if (key == "URL") a.url = val;
        else if (key == "K") a.k = atoi(val.c_str());
        else if (key == "CONTENT_BEGIN") in_content = true;
    }
    return !a.tool.empty();
}

/* ======================================================================
 * Предложенные правки
 * ====================================================================== */

void Engine::pending_apply(size_t idx) {
    std::lock_guard<std::mutex> lk(state_.mtx);
    if (idx >= state_.pending.size()) return;
    const auto& p = state_.pending[idx];
    /* Путь разрешяется относительно проекта. */
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

/* ======================================================================
 * Разрешения на доступ к файлам
 * ====================================================================== */

static bool is_path_outside(const std::string& abs_path, const std::string& project_dir) {
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

static bool is_path_allowed(const std::string& abs_path,
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

std::string Engine::check_external_permission(const std::string& abs_path) {
    if (!is_path_outside(abs_path, state_.project_dir)) return "";
    if (is_path_allowed(abs_path, state_.project_dir, state_.allowed_external_paths)) return "";

    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.pending_permission_path = abs_path;
        state_.waiting_for_permission = true;
    }
    push_event(AgentEvent::Tool, "[access] Требуется разрешение: " + abs_path);
    return "[ВАЖНО] Доступ запрещён. Файл вне проекта: " + abs_path
           + "\nНЕ ПОВТОРЯЙ вызов. Скажи пользователю что нужно нажать «Разрешить»."
           + "\nЖди подтверждения. Не пытайся снова до подтверждения.";
}

void Engine::permission_allow_once(const std::string& path) {
    std::string task_to_retry;
    bool sync_waiting = false;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.allowed_external_paths.push_back(path);
        state_.once_path = path;
        state_.pending_permission_path.clear();
        state_.waiting_for_permission = false;
        sync_waiting = state_.waiting_in_sync;
        task_to_retry = state_.last_agent_task;
    }
    state_.permission_cv.notify_all();
    if (!sync_waiting && !task_to_retry.empty()) {
        submit(task_to_retry);
    }
}

void Engine::permission_allow_always(const std::string& path) {
    std::string task_to_retry;
    bool sync_waiting = false;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.allowed_external_paths.push_back(path);
        state_.pending_permission_path.clear();
        state_.waiting_for_permission = false;
        sync_waiting = state_.waiting_in_sync;
        task_to_retry = state_.last_agent_task;
    }
    state_.permission_cv.notify_all();
    /* Сохраняем на диск. */
    std::string json = "[";
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        for (size_t i = 0; i < state_.allowed_external_paths.size(); ++i) {
            if (i > 0) json += ",";
            json += "\"" + state_.allowed_external_paths[i] + "\"";
        }
    }
    if (cb_.settings_set) cb_.settings_set("wp_coder.allowed_external_paths", json);
    if (!sync_waiting && !task_to_retry.empty()) {
        submit(task_to_retry);
    }
}

void Engine::permission_reject(const std::string& path) {
    std::lock_guard<std::mutex> lk(state_.mtx);
    state_.pending_permission_path.clear();
    state_.waiting_for_permission = false;
    state_.last_agent_task.clear();
    state_.permission_cv.notify_all();
}

/* ======================================================================
 * События
 * ====================================================================== */

void Engine::push_event(AgentEvent::Kind k, const std::string& text) {
    std::lock_guard<std::mutex> lk(state_.mtx);
    state_.events.push_back({k, text});
    if (state_.events.size() > 500) state_.events.pop_front();
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
    state_.php_bin          = setting_get(cb_, "wp_coder.php_bin", "");
    state_.wp_site_url      = setting_get(cb_, "wp_coder.site_url", "");
    state_.wp_app_user      = setting_get(cb_, "wp_coder.app_user", "");
    state_.wp_app_password  = setting_get(cb_, "wp_coder.app_password", "");
    state_.deploy_proto     = setting_get(cb_, "wp_coder.deploy_proto", "rsync");
    state_.deploy_host      = setting_get(cb_, "wp_coder.deploy_host", "");
    state_.deploy_user      = setting_get(cb_, "wp_coder.deploy_user", "");
    state_.deploy_pass      = setting_get(cb_, "wp_coder.deploy_pass", "");
    state_.deploy_port      = setting_get(cb_, "wp_coder.deploy_port", "");
    state_.deploy_remote_dir= setting_get(cb_, "wp_coder.deploy_remote_dir", "");
    state_.wp_local_url    = setting_get(cb_, "wp_coder.local_url", "");
    state_.agent_system_prompt = setting_get(cb_, "wp_coder.agent_system_prompt", "");
    state_.active_module    = setting_get(cb_, "wp_coder.active_module", "");

    /* Загрузка разрешённых внешних путей. */
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
}

} // namespace coder
