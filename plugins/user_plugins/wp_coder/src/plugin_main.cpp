#include "wp_coder.h"

#include "imgui.h"

#include <cstdio>
#include <algorithm>
#include <vector>

/* Глобальные хендлы (используются в tools.cpp / agent.cpp). */
LlamaPluginHost* g_host = nullptr;
const LlamaHostApi* g_api = nullptr;
WpCoderState g_state;

static LlamaPluginWindow* g_win_project = nullptr;
static LlamaPluginWindow* g_win_agent = nullptr;

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

/* --- команды --- */
static void cmd_open_project(void*) {
    if (g_api && g_win_project) g_api->window_set_visible(g_host, g_win_project, 1);
}
static void cmd_open_agent(void*) {
    if (g_api && g_win_agent) g_api->window_set_visible(g_host, g_win_agent, 1);
}

/* --- отрисовка панели Project --- */
static void render_project() {
    if (!g_api->window_is_visible(g_host, g_win_project)) return;
    ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_FirstUseEver);
    ImGui::Begin("WP Coder — Проект");

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
    ImGui::TextDisabled("Корень: %s", g_state.project_dir.c_str());
    ImGui::TextDisabled("php  : %s | deploy: %s", g_state.php_bin.c_str(),
                        g_state.deploy_proto.c_str());

    ImGui::End();
}

/* --- отрисовка панели Agent --- */
static void render_agent() {
    if (!g_api->window_is_visible(g_host, g_win_agent)) return;
    ImGui::SetNextWindowSize(ImVec2(640, 480), ImGuiCond_FirstUseEver);
    ImGui::Begin("WP Coder — Агент");

    /* Лента событий (read-only). */
    ImGui::BeginChild("log", ImVec2(0, -110), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        for (const auto& ev : g_state.events) {
            switch (ev.kind) {
                case AgentEvent::Assistant:
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.9f, 1.0f, 1.0f));
                    break;
                case AgentEvent::Tool:
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 1.0f, 0.7f, 1.0f));
                    break;
                case AgentEvent::Status:
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                    break;
                case AgentEvent::Error:
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
                    break;
            }
            ImGui::TextWrapped("%s", ev.text.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    /* План-режим: список предложенных правок с подтверждением. */
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        bool pm = g_state.plan_mode;
        if (ImGui::Checkbox("План-режим (правки не применяются сразу)", &pm))
            g_state.plan_mode = pm;

        /* Ролевой режим (лёгкая многоАгентность). */
        const char* modes[] = {"Code", "Research", "Review"};
        int m = g_state.mode;
        if (ImGui::Combo("Режим", &m, modes, 3)) g_state.mode = m;

        /* Навыки (skills). */
        if (!g_state.skills.empty()) {
            ImGui::Text("Навыки:");
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
        }

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

    /* Ввод задачи. */
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        ImGui::Text(g_state.running ? "● выполняется…" : "○ ожидание");
    }
    if (ImGui::InputTextMultiline("##prompt", s_prompt, sizeof(s_prompt),
                                  ImVec2(-1, 0),
                                  ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::string p = s_prompt;
        if (!p.empty()) {
            agent_submit(p);
            s_prompt[0] = '\0';
        }
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::SameLine();
    if (ImGui::Button("Отправить")) {
        std::string p = s_prompt;
        if (!p.empty()) {
            agent_submit(p);
            s_prompt[0] = '\0';
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Индексировать (RAG)")) {
        agent_submit("[system] выполни rag_index для текущего проекта");
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

    g_api->command_register(g_host, "wp_coder_open_project", cmd_open_project, nullptr,
                            "WP Coder: Project", "Ctrl+Shift+W");
    g_api->command_register(g_host, "wp_coder_open_agent", cmd_open_agent, nullptr,
                            "WP Coder: Agent", "Ctrl+Shift+E");

    LlamaPluginMenu* menu = g_api->menu_add(g_host, "WordPress");
    if (menu) {
        g_api->menu_add_item(g_host, menu, "Проект", "wp_coder_open_project", "Ctrl+Shift+W");
        g_api->menu_add_item(g_host, menu, "Агент", "wp_coder_open_agent", "Ctrl+Shift+E");
    }

    g_win_project = g_api->window_register(g_host, "wp_coder_project", "WP Coder — Проект");
    g_win_agent   = g_api->window_register(g_host, "wp_coder_agent", "WP Coder — Агент");

    agent_start();
    return 0;
}

LLAMA_PLUGIN_EXPORT void ll_plugin_render(void) {
    if (!g_api || !g_host) return;
    render_project();
    render_agent();
}

LLAMA_PLUGIN_EXPORT void ll_plugin_shutdown(void) {
    agent_stop();
    g_host = nullptr;
    g_api = nullptr;
    g_win_project = g_win_agent = nullptr;
}

} // extern "C"
