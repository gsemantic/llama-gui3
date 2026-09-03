#include "wp_coder.h"

#include <sstream>
#include <vector>
#include <cstring>

/* --- вспомогательные настройки (храним как JSON-строку "\"v\"") --- */
std::string setting_get_str(const std::string& key, const std::string& def) {
    if (!g_api || !g_host) return def;
    char* s = g_api->settings_get(g_host, key.c_str());
    if (!s) return def;
    std::string v(s);
    g_api->free_string(g_host, s);
    if (v.empty()) return def;
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
        v = v.substr(1, v.size() - 2);
    return v;
}

void setting_set_str(const std::string& key, const std::string& value) {
    if (!g_api || !g_host) return;
    std::string j = "\"" + value + "\"";
    g_api->settings_set(g_host, key.c_str(), j.c_str());
}

/* --- протокол инструментов (fenced block, line-based, без JSON-экранирования) ---
 * Модель выводит РОВНО один блок:
 *   ```wp_action
 *   TOOL: read_file
 *   PATH: wp-content/themes/x/functions.php
 *   ```
 * Для write_file контент между CONTENT_BEGIN / CONTENT_END.
 */

const char* kSystemPrompt =
    "Ты — AI-ассистент для работы с WordPress-проектом.\n\n"
    "## ПРОТОКОЛ ВЫЗОВА ИНСТРУМЕНТОВ\n\n"
    "У тебя есть инструменты. Чтобы вызвать инструмент — выведи блок:\n\n"
    "```\nwp_action\nTOOL: имя\nПARAM: значение\n```\n\n"
    "Приложение выполнит команду и вернёт RESULT. Ты НЕ выполняешь сам.\n\n"
    "ПРИМЕРЫ:\n\n"
    "Пользователь: «Покажи содержимое wp-config.php»\n"
    "Твой ответ:\n"
    "```\nwp_action\nTOOL: read_file\nPATH: wp-config.php\n```\n\n"
    "Пользователь: «Проверь зависимости»\n"
    "Твой ответ:\n"
    "```\nwp_action\nTOOL: wp_check_deps\n```\n\n"
    "Пользователь: «Какие хуки в functions.php?»\n"
    "Твой ответ:\n"
    "```\nwp_action\nTOOL: grep_hooks\nROOT: wp-content/themes\n```\n\n"
    "ВАЖНО: ВСЕ пути — ОТНОСИТЕЛЬНЫЕ корня проекта (указан в [КОРНЕВОЙ КАТАЛОГ]).\n"
    "Не используй абсолютные пути — приложение разрешит их автоматически.\n\n"
    "ПРАВИЛА:\n"
    "- РОВНО ОДИН wp_action блок за сообщение\n"
    "- Не пиши «нет доступа» — инструменты работают через wp_action\n"
    "- После получения RESULT — анализируй и продолжай\n"
    "- Когда готово — текстовый ответ БЕЗ wp_action\n\n"
    "## ИНСТРУМЕНТЫ\n\n"
    "read_file     — чтение файла              PATH: <путь>\n"
    "write_file    — запись файла              PATH: <путь> CONTENT_BEGIN ... CONTENT_END\n"
    "grep_hooks    — поиск хуков WP            ROOT: <каталог> PATTERN: <regex>\n"
    "php_lint      — проверка синтаксиса PHP   PATH: <путь>\n"
    "wp_cli        — команда wp-cli            CLI: <аргументы>\n"
    "repo_map      — обзор структуры проекта   ROOT: <каталог>\n"
    "validate      — php -l по всему проекту\n"
    "verify        — проверка (php -l + HTTP + рендер)\n"
    "git_status    — статус git\n"
    "git_diff      — разница с HEAD            PATH: <путь>\n"
    "git_log       — история коммитов          K: <число>\n"
    "git_commit    — коммит                    QUERY: <сообщение>\n"
    "wp_db         — SQL-запрос                QUERY: <SQL>\n"
    "wp_media      — список медиа              K: <число>\n"
    "wp_option     — опция WordPress           QUERY: <имя опции>\n"
    "wp_check_deps — проверка зависимостей (системная команда)\n"
    "wp_create_site — создание WP-сайта        QUERY: <имя_сайта>\n"
    "wp_rest       — REST API                  QUERY: <эндпоинт>\n"
    "rag_index     — индексация в RAG          ROOT: <каталог>\n"
    "rag_query     — поиск в RAG               QUERY: <запрос> K: <число>\n"
    "deploy        — деплой на хостер\n"
    "list_skills   — список навыков\n\n"
    "Пути — относительно корня проекта. АБСОЛЮТНЫЕ пути (/home/...) тоже работают.\n"
    "Ты можешь читать и изменять файлы ЛЮБОЙ части файловой системы,\n"
    "если пользователь об этом просит. Указывай полный путь.\n"
    "При работе с Git соблюдай правила из навыка wp_git.";

namespace {

void run_task(const std::string& task) {
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        g_state.running = true;
    }
    push_event(AgentEvent::Status, "▶ Задача: " + task);

    if (!g_api || !g_host) {
        push_event(AgentEvent::Error, "[ошибка] нет хост-API");
        g_state.running = false;
        return;
    }
    if (g_api->llm_is_connected(g_host) != 1) {
        push_event(AgentEvent::Error,
                   "[ошибка] LLM не подключён (локальный сервер/облако недоступны)");
        g_state.running = false;
        return;
    }

    std::string user = task;
    for (int step = 0; step < 12; ++step) {
        /* Собираем системный промпт: пользовательский или базовый + корень + навыки + режим. */
        std::string sys = g_state.agent_system_prompt.empty()
            ? std::string(kSystemPrompt)
            : g_state.agent_system_prompt;
        {
            std::lock_guard<std::mutex> lk(g_state.mtx);
            if (!g_state.project_dir.empty()) {
                sys += "\n\n## КОРНЕВОЙ КАТАЛОГ ПРОЕКТА\n"
                       "Корень: " + g_state.project_dir + "\n"
                       "Все пути в инструментах — относительно этого каталога.\n"
                       "Если пользователь указывает относительный путь — "
                       "он разрешается относительно корня автоматически.";
            }
            for (const auto& name : g_state.active_skills) {
                for (const auto& sk : g_state.skills) {
                    if (sk.name == name) {
                        sys += "\n\n[НАВЫК: " + sk.name + "]\n" + sk.body;
                        break;
                    }
                }
            }
            if (g_state.mode == 1)
                sys += "\n\n[РЕЖИМ: Research] Только изучай код и отвечай. "
                       "Не меняй файлы и не деплой.";
            else if (g_state.mode == 2)
                sys += "\n\n[РЕЖИМ: Review] После правок обязательно запусти "
                       "verify и доложи о результатах.";
        }

        char* response = nullptr;
        int rc = g_api->llm_complete_ex(g_host, sys.c_str(), user.c_str(), &response);
        if (rc != 1 || !response) {
            push_event(AgentEvent::Error, "[ошибка] LLM не ответил");
            break;
        }
        std::string resp(response);
        g_api->free_string(g_host, response);

        std::string rest;
        std::string block = extract_action(resp, rest);
        if (!rest.empty()) push_event(AgentEvent::Assistant, rest);

        if (block.empty()) {
            push_event(AgentEvent::Status, "■ Готово (финальный ответ).");
            break;
        }

        Action act;
        if (!parse_action(block, act)) {
            push_event(AgentEvent::Error, "[ошибка разбора wp_action] блок:\n" + block);
            break;
        }
        std::string result = tool_run(act.tool, act.path, act.root,
                                      act.query, act.pattern, act.k,
                                      act.content, act.cli, act.url);
        push_event(AgentEvent::Tool, "🔧 " + act.tool + " → " + result);
        user = "RESULT [" + act.tool + "]:\n" + result;
    }
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        g_state.running = false;
    }
}

void worker_main() {
    for (;;) {
        std::string task;
        {
            std::unique_lock<std::mutex> lk(g_state.mtx);
            g_state.cv.wait(lk, [] {
                return g_state.shutting_down || !g_state.inbox.empty();
            });
            if (g_state.shutting_down) break;
            task = std::move(g_state.inbox.front());
            g_state.inbox.pop();
        }
        run_task(task);
    }
}

} // namespace

void push_event(AgentEvent::Kind k, const std::string& text) {
    std::lock_guard<std::mutex> lk(g_state.mtx);
    g_state.events.push_back({k, text});
    if (g_state.events.size() > 500) g_state.events.pop_front();
}

std::string extract_action(const std::string& text, std::string& rest) {
    rest = text;
    // Пробуем два формата: ```wp_action и просто ``` (модели иногда опускают язык)
    size_t a = text.find("```wp_action");
    if (a == std::string::npos) {
        a = text.find("```\nwp_action");
        if (a == std::string::npos) {
            // Пробуем найти純 ``` без языка, но с wp_action в теле
            a = text.find("```");
            if (a == std::string::npos) return "";
            size_t body_check = text.find('\n', a);
            if (body_check == std::string::npos) return "";
            // Проверяем, что после ``` идёт wp_action
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

bool parse_action(const std::string& block, Action& a) {
    std::istringstream iss(block);
    std::string line;
    bool in_content = false;
    while (std::getline(iss, line)) {
        if (in_content) {
            if (line.find("CONTENT_END") != std::string::npos) { in_content = false; continue; }
            if (!a.content.empty()) a.content += '\n';
            a.content += line;
            continue;
        }
        auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);
        /* trim */
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

void agent_start() {
    g_state.worker = std::thread(worker_main);
}

void agent_stop() {
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        g_state.shutting_down = true;
        g_state.cv.notify_all();
    }
    if (g_state.worker.joinable()) g_state.worker.join();
}

void agent_submit(const std::string& prompt) {
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        g_state.inbox.push(prompt);
        g_state.cv.notify_all();
    }
}
