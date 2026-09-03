#include "wp_coder.h"

#include "imgui.h"

#include <cstdio>
#include <iostream>
#include <algorithm>
#include <vector>

/* Глобальные хендлы (используются в tools.cpp / agent.cpp). */
LlamaPluginHost* g_host = nullptr;
const LlamaHostApi* g_api = nullptr;
WpCoderState g_state;

static LlamaPluginWindow* g_win_project = nullptr;
static LlamaPluginWindow* g_win_git = nullptr;
static LlamaPluginWindow* g_win_files = nullptr;

/* Буферы ввода (persistent across frames). */
static char s_project_dir[512] = "";
static char s_php_bin[128] = "";
static char s_site_url[256] = "";
static char s_app_user[128] = "";
static char s_app_password[128] = "";
static char s_deploy_proto[32] = "rsync";
static char s_deploy_host[256] = "";
static char s_deploy_user[128] = "";
static char s_deploy_pass[128] = "";
static char s_deploy_port[16] = "";
static char s_deploy_remote[512] = "";
static char s_local_url[256] = "";
static char s_prompt[2048] = "";
static char s_agent_prompt[8192] = "";
static char s_commit_msg[512] = "";
static char s_file_filter[128] = "";

/* --- команды --- */
static void cmd_open_project(void*) {
    if (g_api && g_win_project) g_api->window_set_visible(g_host, g_win_project, 1);
}
static void cmd_open_git(void*) {
    if (g_api && g_win_git) g_api->window_set_visible(g_host, g_win_git, 1);
}
static void cmd_open_files(void*) {
    if (g_api && g_win_files) g_api->window_set_visible(g_host, g_win_files, 1);
}

/* --- отрисовка панели Project --- */
static void render_project() {
    if (!g_api->window_is_visible(g_host, g_win_project)) return;
    ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("WP Coder — Проект", &open);
    if (!open) g_api->window_set_visible(g_host, g_win_project, 0);

    ImGui::Text("Локальный WordPress (корень, напр. /var/www/html):");
    ImGui::InputText("##projdir", s_project_dir, sizeof(s_project_dir));
    ImGui::SameLine();
    if (ImGui::Button("Сохранить##dir")) {
        g_state.project_dir = s_project_dir;
        project_save_settings();
    }

    ImGui::Text("php-cli:");
    ImGui::InputText("##phpbin", s_php_bin, sizeof(s_php_bin));
    ImGui::SameLine();
    if (ImGui::Button("Авто")) {
        project_detect_php();
        std::snprintf(s_php_bin, sizeof(s_php_bin), "%s", g_state.php_bin.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Сохранить##php")) {
        g_state.php_bin = s_php_bin;
        project_save_settings();
    }

    ImGui::Separator();
    ImGui::Text("Удалённый WP (REST, app_password):");
    ImGui::InputText("Site URL", s_site_url, sizeof(s_site_url));
    ImGui::InputText("Логин", s_app_user, sizeof(s_app_user));
    ImGui::InputText("App password", s_app_password, sizeof(s_app_password));
    if (ImGui::Button("Сохранить##wp")) {
        g_state.wp_site_url = s_site_url;
        g_state.wp_app_user = s_app_user;
        g_state.wp_app_password = s_app_password;
        project_save_settings();
    }

    ImGui::Separator();
    ImGui::Text("Деплой (rsync / ftp / ftps / sftp):");
    ImGui::InputText("Proto", s_deploy_proto, sizeof(s_deploy_proto));
    ImGui::InputText("Host", s_deploy_host, sizeof(s_deploy_host));
    ImGui::InputText("User", s_deploy_user, sizeof(s_deploy_user));
    ImGui::InputText("Pass", s_deploy_pass, sizeof(s_deploy_pass));
    ImGui::InputText("Port", s_deploy_port, sizeof(s_deploy_port));
    ImGui::InputText("Remote dir", s_deploy_remote, sizeof(s_deploy_remote));
    if (ImGui::Button("Сохранить##deploy")) {
        g_state.deploy_proto = s_deploy_proto;
        g_state.deploy_host = s_deploy_host;
        g_state.deploy_user = s_deploy_user;
        g_state.deploy_pass = s_deploy_pass;
        g_state.deploy_port = s_deploy_port;
        g_state.deploy_remote_dir = s_deploy_remote;
        project_save_settings();
    }

    ImGui::Text("Локальный сайт для проверки (http://localhost:порт):");
    ImGui::InputText("##local", s_local_url, sizeof(s_local_url));
    ImGui::SameLine();
    if (ImGui::Button("Сохранить##local")) {
        g_state.wp_local_url = s_local_url;
        project_save_settings();
    }

    ImGui::Separator();
    ImGui::Text("Системный промпт агента (пусто = по умолчанию):");
    if (ImGui::InputTextMultiline("##agent_prompt", s_agent_prompt, sizeof(s_agent_prompt),
                                  ImVec2(-FLT_MIN, 100))) {
        g_state.agent_system_prompt = s_agent_prompt;
        project_save_settings();
    }
    ImGui::SameLine();
    if (ImGui::Button("Сбросить##prompt")) {
        g_state.agent_system_prompt.clear();
        s_agent_prompt[0] = '\0';
        project_save_settings();
    }

    ImGui::Separator();
    ImGui::TextDisabled("Корень: %s", g_state.project_dir.c_str());
    ImGui::TextDisabled("php  : %s | deploy: %s", g_state.php_bin.c_str(),
                        g_state.deploy_proto.c_str());

    ImGui::End();
}

/* --- отрисовка панели Git --- */
static void render_git() {
    if (!g_api->window_is_visible(g_host, g_win_git)) return;
    ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("WP Coder — Git", &open);
    if (!open) g_api->window_set_visible(g_host, g_win_git, 0);

    ImGui::Text("Git-операции для проекта:");
    ImGui::Separator();

    if (ImGui::Button("Git Status")) {
        agent_submit("[system] выполни git_status");
    }
    ImGui::SameLine();
    if (ImGui::Button("Git Diff")) {
        agent_submit("[system] выполни git_diff");
    }
    ImGui::SameLine();
    if (ImGui::Button("Git Log (10)")) {
        agent_submit("[system] выполни git_log");
    }

    ImGui::Separator();
    ImGui::Text("Создать коммит:");
    ImGui::InputText("Сообщение", s_commit_msg, sizeof(s_commit_msg));
    if (ImGui::Button("Git Commit")) {
        std::string msg = s_commit_msg;
        if (!msg.empty()) {
            agent_submit("[system] выполни git_commit с сообщением: " + msg);
            s_commit_msg[0] = '\0';
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("Примечание: git-операции выполняются в корне проекта");

    ImGui::End();
}

/* --- отрисовка панели Файлы --- */
static void render_files() {
    if (!g_api->window_is_visible(g_host, g_win_files)) return;
    ImGui::SetNextWindowSize(ImVec2(500, 600), ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("WP Coder — Файлы", &open);
    if (!open) g_api->window_set_visible(g_host, g_win_files, 0);

    ImGui::Text("Дерево файлов проекта:");
    ImGui::Separator();

    /* Фильтр файлов. */
    ImGui::InputText("Фильтр", s_file_filter, sizeof(s_file_filter));
    ImGui::SameLine();
    if (ImGui::Button("Обновить")) {
        agent_submit("[system] выполни repo_map для текущего проекта");
    }

    ImGui::Separator();

    /* Отображение событий агента (где будут результаты repo_map). */
    ImGui::BeginChild("file_list", ImVec2(0, -110), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        for (const auto& ev : g_state.events) {
            if (ev.kind == AgentEvent::Tool && ev.text.find("repo_map") != std::string::npos) {
                ImGui::TextWrapped("%s", ev.text.c_str());
            }
        }
    }
    ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    /* Быстрые действия. */
    if (ImGui::Button("Открыть в агенте")) {
        agent_submit("[system] выполни repo_map для текущего проекта");
    }
    ImGui::SameLine();
    if (ImGui::Button("Прочитать файл")) {
        agent_submit("[system] введи путь к файлу для чтения");
    }

    ImGui::End();
}

/* --- экспортируемые функции --- */
extern "C" {

LLAMA_PLUGIN_EXPORT const char* ll_plugin_api_version(void) {
    return LLAMA_PLUGIN_API_VERSION;
}

LLAMA_PLUGIN_EXPORT const LlamaPluginInfo* ll_plugin_info(void) {
    static const LlamaPluginInfo info = {
        "wp_coder",
        "0.1.0",
        "AI-кодер для WordPress: темы, плагины, хуки, RAG, локалхост",
        "llama-gui"
    };
    return &info;
}

/* --- Agent mode callbacks (для интеграции с основным чатом) --- */

static char* agent_mode_on_message(LlamaPluginHost* host, const char* user_message, void* user_data) {
    if (!user_message || !user_message[0]) return nullptr;

    /* Запускаем ReAct-цикл синхронно и собираем весь ответ. */
    std::string task = user_message;
    std::string full_response;
    std::string sys_prompt = g_state.agent_system_prompt.empty()
        ? std::string(kSystemPrompt)
        : g_state.agent_system_prompt;
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        if (!g_state.project_dir.empty()) {
            sys_prompt += "\n\n## КОРНЕВОЙ КАТАЛОГ ПРОЕКТА\n"
                          "Корень: " + g_state.project_dir + "\n"
                          "Все пути в инструментах — относительно этого каталога.\n"
                          "Если пользователь указывает относительный путь — "
                          "он разрешается относительно корня автоматически.";
        }
        for (const auto& name : g_state.active_skills) {
            for (const auto& sk : g_state.skills) {
                if (sk.name == name) {
                    sys_prompt += "\n\n[НАВЫК: " + sk.name + "]\n" + sk.body;
                    break;
                }
            }
        }
        if (g_state.mode == 1)
            sys_prompt += "\n\n[РЕЖИМ: Research] Только изучай код и отвечай. Не меняй файлы и не деплой.";
        else if (g_state.mode == 2)
            sys_prompt += "\n\n[РЕЖИМ: Review] После правок обязательно запусти verify и доложи о результатах.";
    }

    for (int step = 0; step < 12; ++step) {
        std::cerr << "[WP Agent] step=" << step << " task=" << task.substr(0, 120)
                  << " project_dir=" << g_state.project_dir << std::endl;
        if (!g_api || !g_host) break;
        if (g_api->llm_is_connected(g_host) != 1) {
            full_response = "[ошибка] LLM не подключён";
            std::cerr << "[WP Agent] LLM not connected" << std::endl;
            break;
        }
        char* response = nullptr;
        int rc = g_api->llm_complete_ex(g_host, sys_prompt.c_str(), task.c_str(), &response);
        if (rc != 1 || !response) {
            full_response = "[ошибка] LLM не ответил";
            std::cerr << "[WP Agent] llm_complete_ex rc=" << rc << std::endl;
            break;
        }
        std::string resp(response);
        g_api->free_string(g_host, response);
        std::cerr << "[WP Agent] response size=" << resp.size() << std::endl;
        std::cerr << "[WP Agent] response: " << resp.substr(0, 300) << std::endl;

        std::string rest;
        std::string block = extract_action(resp, rest);
        std::cerr << "[WP Agent] block=" << block.substr(0, 200)
                  << " rest=" << rest.substr(0, 200) << std::endl;
        if (!rest.empty()) {
            if (!full_response.empty()) full_response += "\n\n";
            full_response += rest;
        }
        if (block.empty()) {
            std::cerr << "[WP Agent] no wp_action block, breaking" << std::endl;
            break;
        }

        Action act;
        if (!parse_action(block, act)) {
            full_response += "\n\n[ошибка разбора wp_action]";
            std::cerr << "[WP Agent] parse_action failed" << std::endl;
            break;
        }
        std::cerr << "[WP Agent] tool=" << act.tool << " path=" << act.path << std::endl;
        std::string result = tool_run(act.tool, act.path, act.root,
                                      act.query, act.pattern, act.k,
                                      act.content, act.cli, act.url);
        std::cerr << "[WP Agent] tool result size=" << result.size() << std::endl;
        task = "RESULT [" + act.tool + "]:\n" + result;
    }
    std::cerr << "[WP Agent] done. full_response size=" << full_response.size() << std::endl;

    if (full_response.empty()) full_response = "(пустой ответ)";
    char* out = (char*)malloc(full_response.size() + 1);
    if (out) memcpy(out, full_response.c_str(), full_response.size() + 1);
    return out;
}

static void agent_mode_render_extras(LlamaPluginHost* host, void* user_data) {
    if (!g_api || !g_host) return;

    /* План-режим. */
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        bool pm = g_state.plan_mode;
        if (ImGui::Checkbox("План-режим (правки не применяются сразу)", &pm))
            g_state.plan_mode = pm;
    }

    /* Ролевой режим. */
    const char* modes[] = {"Code", "Research", "Review"};
    int m = g_state.mode;
    if (ImGui::Combo("Режим", &m, modes, 3)) g_state.mode = m;

    /* Навыки (сворачиваемый блок). */
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        if (!g_state.skills.empty()) {
            if (ImGui::TreeNode("Навыки")) {
                for (size_t i = 0; i < g_state.skills.size(); ++i) {
                    bool on = std::find(g_state.active_skills.begin(),
                                        g_state.active_skills.end(),
                                        g_state.skills[i].name) != g_state.active_skills.end();
                    if (ImGui::Checkbox(("##sk" + std::to_string(i)).c_str(), &on)) {
                        auto& v = g_state.active_skills;
                        auto it = std::find(v.begin(), v.end(), g_state.skills[i].name);
                        if (on && it == v.end()) v.push_back(g_state.skills[i].name);
                        else if (!on && it != v.end()) v.erase(it);
                    }
                    ImGui::SameLine();
                    ImGui::Text("%s", g_state.skills[i].name.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", g_state.skills[i].description.c_str());
                }
                ImGui::TreePop();
            }
        }
    }

    /* Предложенные правки. */
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        if (!g_state.pending.empty()) {
            ImGui::Text("Предложенные правки (%zu):", g_state.pending.size());
            for (size_t i = 0; i < g_state.pending.size(); ++i) {
                ImGui::BulletText("%s", g_state.pending[i].path.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton(("Применить##a"+std::to_string(i)).c_str())) {
                    pending_apply(i);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(("Отклонить##d"+std::to_string(i)).c_str())) {
                    pending_discard(i);
                }
            }
        }
    }
}

LLAMA_PLUGIN_EXPORT int ll_plugin_init(LlamaPluginHost* host, const LlamaHostApi* api) {
    if (!host || !api) return 1;
    g_host = host;
    g_api = api;

    project_load_settings();
    project_detect_php();
    skills_load();
    std::snprintf(s_project_dir, sizeof(s_project_dir), "%s", g_state.project_dir.c_str());
    std::snprintf(s_php_bin, sizeof(s_php_bin), "%s", g_state.php_bin.c_str());
    std::snprintf(s_site_url, sizeof(s_site_url), "%s", g_state.wp_site_url.c_str());
    std::snprintf(s_app_user, sizeof(s_app_user), "%s", g_state.wp_app_user.c_str());
    std::snprintf(s_app_password, sizeof(s_app_password), "%s", g_state.wp_app_password.c_str());
    std::snprintf(s_deploy_proto, sizeof(s_deploy_proto), "%s", g_state.deploy_proto.c_str());
    std::snprintf(s_deploy_host, sizeof(s_deploy_host), "%s", g_state.deploy_host.c_str());
    std::snprintf(s_deploy_user, sizeof(s_deploy_user), "%s", g_state.deploy_user.c_str());
    std::snprintf(s_deploy_pass, sizeof(s_deploy_pass), "%s", g_state.deploy_pass.c_str());
    std::snprintf(s_deploy_port, sizeof(s_deploy_port), "%s", g_state.deploy_port.c_str());
    std::snprintf(s_deploy_remote, sizeof(s_deploy_remote), "%s", g_state.deploy_remote_dir.c_str());
    std::snprintf(s_local_url, sizeof(s_local_url), "%s", g_state.wp_local_url.c_str());
    std::snprintf(s_agent_prompt, sizeof(s_agent_prompt), "%s", g_state.agent_system_prompt.c_str());

    g_api->command_register(g_host, "wp_coder_open_project", cmd_open_project, nullptr,
                            "WP Coder: Project", "Ctrl+Shift+W");
    g_api->command_register(g_host, "wp_coder_open_git", cmd_open_git, nullptr,
                            "WP Coder: Git", "Ctrl+Shift+G");
    g_api->command_register(g_host, "wp_coder_open_files", cmd_open_files, nullptr,
                            "WP Coder: Files", "Ctrl+Shift+F");

    LlamaPluginMenu* menu = g_api->menu_add(g_host, "WordPress");
    if (menu) {
        g_api->menu_add_item(g_host, menu, "Проект", "wp_coder_open_project", "Ctrl+Shift+W");
        g_api->menu_add_item(g_host, menu, "Git", "wp_coder_open_git", "Ctrl+Shift+G");
        g_api->menu_add_item(g_host, menu, "Файлы", "wp_coder_open_files", "Ctrl+Shift+F");
    }

    g_win_project = g_api->window_register(g_host, "wp_coder_project", "WP Coder — Проект");
    g_win_git     = g_api->window_register(g_host, "wp_coder_git", "WP Coder — Git");
    g_win_files   = g_api->window_register(g_host, "wp_coder_files", "WP Coder — Файлы");

    /* Регистрируем режим агента для основного чата. */
    static LlamaPluginAgentMode agent_mode = {};
    agent_mode.name = "wp_coder";
    agent_mode.display_name = "WP Agent";
    agent_mode.on_message = agent_mode_on_message;
    agent_mode.render_extras = agent_mode_render_extras;
    agent_mode.user_data = nullptr;
    g_api->agent_mode_register(g_host, &agent_mode);

    agent_start();
    return 0;
}

LLAMA_PLUGIN_EXPORT void ll_plugin_render(void) {
    if (!g_api || !g_host) return;
    render_project();
    render_git();
    render_files();
}

LLAMA_PLUGIN_EXPORT void ll_plugin_shutdown(void) {
    agent_stop();
    g_host = nullptr;
    g_api = nullptr;
    g_win_project = g_win_git = g_win_files = nullptr;
}

} // extern "C"
