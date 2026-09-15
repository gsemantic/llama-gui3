#include "coder_window.h"
#include "../core/engine.h"
#include "../core/tools_registry.h"
#include "../core/skills_manager.h"
#include "../core/module_api.h"
#include "../core/project.h"
#include "../core/security.h"

#include "imgui.h"
#include "plugins/plugin_api.h"

#include <cstring>
#include <algorithm>
#include <sstream>

/* Глобальные хендлы хоста (из plugin_main.cpp). */
extern LlamaPluginHost* g_host;
extern const LlamaHostApi* g_api;

namespace coder {
namespace ui {

/* Окна. */
static LlamaPluginWindow* g_win_project = nullptr;
static LlamaPluginWindow* g_win_modules = nullptr;
static LlamaPluginWindow* g_win_tools = nullptr;
static LlamaPluginWindow* g_win_session = nullptr;

/* Буферы ввода. */
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
static char s_agent_prompt[8192] = "";

/* Инициализация буферов из состояния. */
void init_buffers() {
    auto& st = engine_state();
    std::snprintf(s_project_dir, sizeof(s_project_dir), "%s", st.project_dir.c_str());
    std::snprintf(s_php_bin, sizeof(s_php_bin), "%s", st.php_bin.c_str());
    std::snprintf(s_site_url, sizeof(s_site_url), "%s", st.wp_site_url.c_str());
    std::snprintf(s_app_user, sizeof(s_app_user), "%s", st.wp_app_user.c_str());
    std::snprintf(s_app_password, sizeof(s_app_password), "%s", st.wp_app_password.c_str());
    std::snprintf(s_deploy_proto, sizeof(s_deploy_proto), "%s", st.deploy_proto.c_str());
    std::snprintf(s_deploy_host, sizeof(s_deploy_host), "%s", st.deploy_host.c_str());
    std::snprintf(s_deploy_user, sizeof(s_deploy_user), "%s", st.deploy_user.c_str());
    std::snprintf(s_deploy_pass, sizeof(s_deploy_pass), "%s", st.deploy_pass.c_str());
    std::snprintf(s_deploy_port, sizeof(s_deploy_port), "%s", st.deploy_port.c_str());
    std::snprintf(s_deploy_remote, sizeof(s_deploy_remote), "%s", st.deploy_remote_dir.c_str());
    std::snprintf(s_local_url, sizeof(s_local_url), "%s", st.wp_local_url.c_str());
    std::snprintf(s_agent_prompt, sizeof(s_agent_prompt), "%s", st.agent_system_prompt.c_str());
}

/* Скопировать UI-буферы в состояние движка и сохранить.
 * Общая реализация для всех кнопок «Сохранить» (Фаза 4.8 — убрано дублирование). */
static void apply_settings_from_buffers() {
    auto& st = engine_state();
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        st.project_dir = s_project_dir;
        st.php_bin = s_php_bin;
        st.wp_site_url = s_site_url;
        st.wp_app_user = s_app_user;
        st.wp_app_password = s_app_password;
        st.deploy_proto = s_deploy_proto;
        st.deploy_host = s_deploy_host;
        st.deploy_user = s_deploy_user;
        st.deploy_pass = s_deploy_pass;
        st.deploy_port = s_deploy_port;
        st.deploy_remote_dir = s_deploy_remote;
        st.wp_local_url = s_local_url;
    }
    engine().invalidate_prompt_cache();
    engine().save_settings();
}

/* Команды. */
static void cmd_open_project(void*) {
    if (g_api && g_win_project) g_api->window_set_visible(g_host, g_win_project, 1);
}
static void cmd_open_modules(void*) {
    if (g_api && g_win_modules) g_api->window_set_visible(g_host, g_win_modules, 1);
}
static void cmd_open_tools(void*) {
    if (g_api && g_win_tools) g_api->window_set_visible(g_host, g_win_tools, 1);
}
static void cmd_open_session(void*) {
    if (g_api && g_win_session) g_api->window_set_visible(g_host, g_win_session, 1);
}

/* --- Окно: Проект --- */
static void render_project() {
    if (!g_api->window_is_visible(g_host, g_win_project)) return;
    ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("AI Coder — Проект", &open);
    if (!open) g_api->window_set_visible(g_host, g_win_project, 0);

    auto& st = engine_state();

    ImGui::Text("Корневой каталог проекта:");
    ImGui::InputText("##projdir", s_project_dir, sizeof(s_project_dir));
    ImGui::SameLine();
    if (ImGui::Button("Сохранить##dir")) {
        if (!coder::security::is_project_dir_valid(s_project_dir)) {
            /* Путь запрещён — не сохраняем. */
        } else {
            apply_settings_from_buffers();
        }
    }
    /* Предупреждение о небезопасном пути. */
    if (!coder::security::is_project_dir_valid(s_project_dir) && s_project_dir[0] != '\0') {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
            "! Небезопасный путь — выберите подкаталог проекта");
    }

    ImGui::Text("php-cli:");
    ImGui::InputText("##phpbin", s_php_bin, sizeof(s_php_bin));
    ImGui::SameLine();
    if (ImGui::Button("Авто")) {
        project_detect_php();
        std::snprintf(s_php_bin, sizeof(s_php_bin), "%s", st.php_bin.c_str());
    }

    ImGui::Separator();
    ImGui::Text("Удалённый WP (REST, app_password):");
    ImGui::InputText("Site URL", s_site_url, sizeof(s_site_url));
    ImGui::InputText("Логин", s_app_user, sizeof(s_app_user));
    ImGui::InputText("App password", s_app_password, sizeof(s_app_password));

    ImGui::Separator();
    ImGui::Text("Деплой:");
    ImGui::InputText("Proto", s_deploy_proto, sizeof(s_deploy_proto));
    ImGui::InputText("Host", s_deploy_host, sizeof(s_deploy_host));
    ImGui::InputText("User", s_deploy_user, sizeof(s_deploy_user));
    ImGui::InputText("Pass", s_deploy_pass, sizeof(s_deploy_pass));
    ImGui::InputText("Port", s_deploy_port, sizeof(s_deploy_port));
    ImGui::InputText("Remote dir", s_deploy_remote, sizeof(s_deploy_remote));

    ImGui::Text("Локальный сайт:");
    ImGui::InputText("##local", s_local_url, sizeof(s_local_url));

    ImGui::Separator();
    ImGui::Text("Системный промпт агента:");
    if (ImGui::InputTextMultiline("##agent_prompt", s_agent_prompt, sizeof(s_agent_prompt),
                                  ImVec2(-FLT_MIN, 80))) {
        st.agent_system_prompt = s_agent_prompt;
        engine().invalidate_prompt_cache();
        engine().save_settings();
    }
    if (ImGui::Button("Сбросить промпт")) {
        st.agent_system_prompt.clear();
        s_agent_prompt[0] = '\0';
        engine().invalidate_prompt_cache();
        engine().save_settings();
    }

    ImGui::Separator();
    ImGui::TextDisabled("Корень: %s", st.project_dir.c_str());

    /* Настройки агента (5.3): лимиты, переопределяют дефолты core/limits.h. */
    ImGui::Separator();
    ImGui::Text("Агент:");
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        int steps = st.max_steps;
        if (ImGui::InputInt("Лимит шагов", &steps, 1, 5)) {
            if (steps < 1) steps = 1;
            if (steps > 100) steps = 100;
            st.max_steps = steps;
            engine().save_settings();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Максимум шагов ReAct-цикла на задачу (по умолчанию 12)");
        int budget_kb = (int)(st.session_budget / 1024);
        if (ImGui::InputInt("Бюджет сессии (КБ)", &budget_kb, 8, 64)) {
            if (budget_kb < 8) budget_kb = 8;
            st.session_budget = (size_t)budget_kb * 1024;
            engine().save_settings();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Бюджет символов истории сессии — при превышении старые RESULT сжимаются");
    }

    ImGui::Spacing();
    if (ImGui::Button("Сохранить все настройки")) {
        apply_settings_from_buffers();
    }

    ImGui::End();
}

/* --- Окно: Модули --- */
static void render_modules() {
    if (!g_api->window_is_visible(g_host, g_win_modules)) return;
    ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("AI Coder — Модули", &open);
    if (!open) g_api->window_set_visible(g_host, g_win_modules, 0);

    auto& st = engine_state();
    const auto& modules = ModuleRegistry::instance().modules();

    ImGui::Text("Активный модуль:");
    ImGui::Separator();

    for (const auto* mod : modules) {
        bool active = (st.active_module == mod->name);
        if (ImGui::RadioButton(mod->display_name, active)) {
            st.active_module = mod->name;
            SkillsManager::instance().set_module(mod->name);
            engine().invalidate_prompt_cache();
            engine().save_settings();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", mod->description);
    }

    if (modules.empty()) {
        ImGui::TextDisabled("Нет зарегистрированных модулей");
    }

    ImGui::Separator();
    ImGui::Text("Инструментов: %zu", ToolsRegistry::instance().list_tools().size());
    ImGui::Text("Навыков: %zu", SkillsManager::instance().all_skills().size());

    ImGui::End();
}

/* --- Окно: Инструменты --- */
static void render_tools() {
    if (!g_api->window_is_visible(g_host, g_win_tools)) return;
    ImGui::SetNextWindowSize(ImVec2(400, 400), ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("AI Coder — Инструменты", &open);
    if (!open) g_api->window_set_visible(g_host, g_win_tools, 0);

    auto tools = ToolsRegistry::instance().list_tools();
    ImGui::Text("Зарегистрированные инструменты (%zu):", tools.size());
    ImGui::Separator();

    ImGui::BeginChild("tools_list", ImVec2(0, -110), ImGuiChildFlags_Borders);
    for (const auto& t : tools) {
        ImGui::BulletText("%s", t.c_str());
    }
    ImGui::EndChild();

    ImGui::Text("Навыки (включены в промпт):");
    const auto& active = SkillsManager::instance().active_skills();
    for (const auto& name : active) {
        ImGui::BulletText("%s", name.c_str());
    }

    ImGui::End();
}

/* --- Окно: Сессия (5.1) --- */
static void render_session() {
    if (!g_api->window_is_visible(g_host, g_win_session)) return;
    ImGui::SetNextWindowSize(ImVec2(560, 480), ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("AI Coder — Сессия", &open);
    if (!open) g_api->window_set_visible(g_host, g_win_session, 0);

    auto& st = engine_state();

    /* Статус FSM + метрики. */
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        ImGui::Text("Состояние: %s", coder::agent_state_name(st.state));
        char stats[200];
        std::snprintf(stats, sizeof(stats),
            "токены: %d | скорость: %.1f tok/s | LLM: %.1fs | шаги: %d",
            st.total_completion_tokens, st.last_tokens_per_second,
            st.llm_total_time, st.steps);
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 0.6f, 1.0f), "%s", stats);
        if (st.state != AgentState::Idle && ImGui::Button("Стоп", {-1, 0})) {
            engine().request_abort();
        }
        ImGui::Separator();
    }

    /* Лента событий (последние 40). */
    ImGui::BeginChild("session_events", ImVec2(0, 0), ImGuiChildFlags_Borders);
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        size_t from = st.events.size() > 40 ? st.events.size() - 40 : 0;
        for (size_t i = from; i < st.events.size(); ++i) {
            const auto& e = st.events[i];
            switch (e.kind) {
                case AgentEvent::Assistant:
                    ImGui::TextColored(ImVec4(0.8f, 0.8f, 1.0f, 1.0f), "%s", e.text.c_str());
                    break;
                case AgentEvent::Tool:
                    ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.6f, 1.0f), "%s", e.text.c_str());
                    break;
                case AgentEvent::Status:
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", e.text.c_str());
                    break;
                case AgentEvent::Error:
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", e.text.c_str());
                    break;
            }
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

/* --- Инициализация --- */
void init_windows() {
    if (!g_api || !g_host) return;

    g_api->command_register(g_host, "ai_coder_open_project", cmd_open_project, nullptr,
                            "AI Coder: Project", "Ctrl+Shift+W");
    g_api->command_register(g_host, "ai_coder_open_modules", cmd_open_modules, nullptr,
                            "AI Coder: Modules", "Ctrl+Shift+M");
    g_api->command_register(g_host, "ai_coder_open_tools", cmd_open_tools, nullptr,
                            "AI Coder: Tools", "Ctrl+Shift+T");
    g_api->command_register(g_host, "ai_coder_open_session", cmd_open_session, nullptr,
                            "AI Coder: Session", "Ctrl+Shift+S");

    LlamaPluginMenu* menu = g_api->menu_add(g_host, "AI Coder");
    if (menu) {
        g_api->menu_add_item(g_host, menu, "Проект", "ai_coder_open_project", "Ctrl+Shift+W");
        g_api->menu_add_item(g_host, menu, "Модули", "ai_coder_open_modules", "Ctrl+Shift+M");
        g_api->menu_add_item(g_host, menu, "Инструменты", "ai_coder_open_tools", "Ctrl+Shift+T");
        g_api->menu_add_item(g_host, menu, "Сессия", "ai_coder_open_session", "Ctrl+Shift+S");
    }

    g_win_project = g_api->window_register(g_host, "ai_coder_project", "AI Coder — Проект");
    g_win_modules = g_api->window_register(g_host, "ai_coder_modules", "AI Coder — Модули");
    g_win_tools   = g_api->window_register(g_host, "ai_coder_tools", "AI Coder — Инструменты");
    g_win_session = g_api->window_register(g_host, "ai_coder_session", "AI Coder — Сессия");

    init_buffers();
}

/* --- Рендер --- */
void render_all_windows() {
    render_project();
    render_modules();
    render_tools();
    render_session();
}

void render_extras() {
    auto& st = engine_state();

/* Индикатор статуса агента + кнопка Стоп. */
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        if (st.state != coder::AgentState::Idle) {
            float t = (float)ImGui::GetTime();
            const char spinner[] = "|/-\\";
            int idx = (int)(t * 4.0f) % 4;
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f),
                "%c Агент работает... (%s)", spinner[idx],
                coder::agent_state_name(st.state));
            if ( ImGui::Button("Стоп", {-1, 0}) ) {
                engine().request_abort();
            }
        } else if (st.last_response_time > 0) {
            char stats[160];
            std::snprintf(stats, sizeof(stats),
                "%d tok | %.1f tok/s | %ds (LLM %.1fs) | %d steps",
                st.last_tokens_generated,
                st.last_tokens_per_second,
                (int)st.last_response_time,
                (int)st.llm_total_time,
                st.steps);
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 0.6f, 1.0f), "%s", stats);
        }
    }

    /* План-режим. */
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        bool pm = st.plan_mode;
        if (ImGui::Checkbox("План-режим (правки не применяются сразу)", &pm))
            st.plan_mode = pm;
    }

    /* Продолжение сессии и очистка. */
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        bool cc = st.continue_conversation;
        if (ImGui::Checkbox("Продолжать сессию", &cc)) {
            st.continue_conversation = cc;
            engine().save_settings();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Если включено — план и контекст сохраняются\nмежду сообщениями. Если нет — каждый запрос\nначинается заново.");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Очистить сессию")) {
        engine().clear_session();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Сбросить план и историю диалога.\nСледующий запрос начнётся с чистого листа.");

    /* Размер сессии. */
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        if (!st.session.empty()) {
            size_t total = 0;
            for (const auto& m : st.session) total += m.content.size();
            ImGui::SameLine();
            ImGui::TextDisabled("ctx: %zuK / %zuK",
                total / 1024, st.session_budget / 1024);
        }
    }

    /* Режим агента. */
    const char* modes[] = {"Code", "Research", "Review"};
    int m = st.mode;
    if (ImGui::Combo("Режим", &m, modes, 3)) {
        st.mode = m;
        engine().invalidate_prompt_cache();
    }

    /* Навыки. */
    {
        const auto& skills = SkillsManager::instance().all_skills();
        if (!skills.empty()) {
            if (ImGui::TreeNode("Навыки")) {
                for (size_t i = 0; i < skills.size(); ++i) {
                    bool on = std::find(SkillsManager::instance().active_skills().begin(),
                                        SkillsManager::instance().active_skills().end(),
                                        skills[i].name) != SkillsManager::instance().active_skills().end();
                    if (ImGui::Checkbox(("##sk" + std::to_string(i)).c_str(), &on)) {
                        SkillsManager::instance().toggle(skills[i].name, on);
                        engine().invalidate_prompt_cache();
                    }
                    ImGui::SameLine();
                    ImGui::Text("%s", skills[i].name.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", skills[i].description.c_str());
                }
                ImGui::TreePop();
            }
        }
    }

    /* Предложенные правки. */
    {
        /* Копия под мьютексом: worker-поток может менять st.pending между
         * кадрами — работаем только с целостным снимком (Фаза 4.7). */
        std::vector<coder::PendingWrite> pending_copy;
        {
            std::lock_guard<std::mutex> lk(st.mtx);
            pending_copy = st.pending;
        }
        if (!pending_copy.empty()) {
            ImGui::Text("Предложенные правки (%zu):", pending_copy.size());
            for (size_t i = 0; i < pending_copy.size(); ++i) {
                ImGui::BulletText("%s", pending_copy[i].path.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton(("OK##a"+std::to_string(i)).c_str())) {
                    engine().pending_apply(i);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(("X##d"+std::to_string(i)).c_str())) {
                    engine().pending_discard(i);
                }
            }
        }
    }

    /* Диалог разрешения доступа. */
    {
        std::string perm_path;
        {
            std::lock_guard<std::mutex> lk(st.mtx);
            perm_path = st.pending_permission_path;
        }
        if (!perm_path.empty()) {
            ImGui::Separator();
            ImGui::Text("Доступ за пределами проекта:");
            ImGui::TextWrapped("%s", perm_path.c_str());
            if (ImGui::SmallButton("Разрешить (один раз)")) {
                engine().permission_allow_once(perm_path);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Разрешить (всегда)")) {
                engine().permission_allow_always(perm_path);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Отклонить")) {
                engine().permission_reject(perm_path);
            }
        }
    }
}

} // namespace ui
} // namespace coder
