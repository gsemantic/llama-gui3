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
namespace {

const char* kSystemPrompt =
    "Ты — AI-кодер для WordPress (темы, плагины, хуки, REST, опции). "
    "Работаешь с локальным сайтом на диске. Когда нужно прочитать/изменить файл, "
    "найти хук или проверить синтаксис — используй инструменты. "
    "Доступные инструменты (выводи РОВНО один блок ```wp_action ... ``` за раз):\n"
    "  TOOL: read_file        PATH: <отн. путь>\n"
    "  TOOL: write_file       PATH: <отн. путь>\n"
    "      CONTENT_BEGIN\n<полное содержимое файла>\nCONTENT_END\n"
    "  TOOL: grep_hooks       ROOT: <отн. каталог, опц.>  PATTERN: <regex по имени хука, опц.>\n"
    "  TOOL: php_lint         PATH: <отн. путь>\n"
    "  TOOL: wp_cli           CLI: <аргументы wp-cli, напр. 'plugin list'>\n"
    "  TOOL: headless_render  URL: <http(s)-адрес страницы для проверки DOM>\n"
    "  TOOL: rag_index        ROOT: <отн. каталог, опц.>      (проиндексировать php в RAG)\n"
    "  TOOL: rag_query        QUERY: <запрос>  K: <число, опц.>\n"
    "  TOOL: repo_map         ROOT: <отн. каталог, опц.>      (компактный обзор проекта)\n"
    "  TOOL: wp_rest          QUERY: <эндпоинт wp/v2, напр. 'posts?per_page=3'>\n"
    "  TOOL: validate         (php -l по всему проекту, ловит синтаксис)\n"
    "  TOOL: verify           (авто-проверка: php -l + HTTP-статус + рендер лок. сайта)\n"
    "  TOOL: deploy           (пуш на хостер: rsync или внешний deploy.sh)\n"
    "  TOOL: list_skills      (показать доступные навыки)\n"
    "Пути — относительно корня проекта. После каждого вызова тебе вернут RESULT, "
    "анализируй его и продолжай, пока не дашь финальный ответ без блока wp_action.";

void push_event(AgentEvent::Kind k, const std::string& text) {
    std::lock_guard<std::mutex> lk(g_state.mtx);
    g_state.events.push_back({k, text});
    if (g_state.events.size() > 500) g_state.events.pop_front();
}

std::string extract_action(const std::string& text, std::string& rest) {
    rest = text;
    size_t a = text.find("```wp_action");
    if (a == std::string::npos) return "";
    size_t body = text.find('\n', a);
    if (body == std::string::npos) return "";
    size_t b = text.find("```", body);
    if (b == std::string::npos) return "";
    std::string block = text.substr(body + 1, b - body - 1);
    rest = text.substr(0, a) + text.substr(b + 3);
    return block;
}

/* Разбор блока wp_action в поля. */
struct Action {
    std::string tool, path, root, query, pattern, content, cli, url;
    int k = 6;
};

bool parse_action(const std::string& block, Action& a) {
    std::istringstream iss(block);
    std::string line;
    bool in_content = false;
    while (std::getline(iss, line)) {
        if (in_content) {
            if (line == "CONTENT_END") in_content = false;
            else a.content += line + "\n";
            continue;
        }
        size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string key = line.substr(0, c);
        std::string val = line.substr(c + 1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(0, 1);
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
        /* Собираем системный промпт: база + активные навыки + режим. */
        std::string sys = kSystemPrompt;
        {
            std::lock_guard<std::mutex> lk(g_state.mtx);
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
