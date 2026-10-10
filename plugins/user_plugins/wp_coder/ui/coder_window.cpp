#include "coder_window.h"
#include "../core/engine.h"
#include "../core/tools_registry.h"
#include "../core/skills_manager.h"
#include "../core/module_api.h"
#include "../core/project.h"
#include "../core/security.h"
#include "../core/session_store.h"
#include "../core/snapshot.h"
#include "../core/timeline.h"
#include "../core/tool_display.h"
#include "../core/stream_panel.h"
#include "../core/todo_panel.h"
#include "../core/permission_panel.h"

#include "imgui.h"
#include "plugins/plugin_api.h"

#include <cstring>
#include <map>
#include <mutex>
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
    /* И9.1: корень проекта входит в состав инструкций (AGENTS.md /
     * CLAUDE.md лежат именно там), поэтому смена корня обязана
     * перечитать их. Отдельно от invalidate_prompt_cache(): тот зовётся
     * ещё и на каждом todowrite, а перечитывать инструкции посреди хода
     * нельзя — это сетевой запрос к URL из настройки. */
    engine().reload_instructions();
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
/* И10.5: diff по уровням стека сессии. И11.1: виджет diff.
 *
 * Задача 10.5 — «diff для каждого снапшота доступен UI», и здесь ровно
 * «доступен»: список уровней, по кнопке — что изменит отмена этого
 * уровня. Дальше И11.1 заменила простой текст на виджет: строки с тематизацией
 * по виду, номера с разной окраской и переключатель переноса.
 *
 * Рисование — тонкий слой. Всё решение (разбор патча на строки с видом и
 * номерами, срезание общего отступа, цвет по виду, ширина колонки номеров и
 * ширина содержимого) лежит в core/diff.h и покрыто проверками, потому что
 * тестового харнесса для ImGui нет, а 11.13 будет сверять именно готовую
 * строку. Здесь остаётся три вещи: позвать core, покрасить и напечатать.
 *
 * Два правила, которые видны в этой функции и обязаны быть названы:
 *   - `state_.mtx` НЕ держится на время подсчёта: снимки читаются под
 *     локом, а diff считает Engine::level_diff уже без него (правило 3
 *     SESSION_START, тот же висящий UI, что закрыл D1);
 *   - отчёт с `truncated` показывается как НЕПОЛНЫЙ, и пропущенные
 *     файлы называются числом. Молчаливое усечение выглядело бы как
 *     «изменилось вот столько», и человек нажал бы «отменить» не
 *     зная, что вернётся. */
static bool s_diff_loaded = false;
static size_t s_diff_index = 0;
static snapshot::DiffReport s_diff_report;
/* Перенос строк в виджете diff. По умолчанию ВКЛЮЧЁН, и это решение, а не
 * умолчание ImGui: без переноса длинная строка обрезается по краю окна, и
 * конец правки не виден вовсе — то есть человек нажимает «отменить», не
 * прочитав того, что отменит. */
static bool s_diff_word_wrap = true;

/* Строки одного файла: тематизация по виду, номера, перенос.
 *
 * Ширина содержимого задаётся по самой широкой строке блока, и это нужно
 * только БЕЗ переноса: строка должна помещаться целиком, иначе полоса
 * прокрутки не появится и конец правки просто пропадёт. */
static void render_diff_rows(const coder::diff::FileDiff& fd) {
    std::vector<coder::diff::DiffRow> rows =
        coder::diff::parse_unified(fd.patch);
    const size_t trimmed = coder::diff::trim_diff(rows);
    const coder::diff::DiffGutter gutter = coder::diff::diff_number_gutter(rows);
    /* И11.2: блоки правки. Их вид («замена», «вставка», «удаление») и полоса
     * под строкой решаются в core: подсветка должна отличать ЗАМЕНУ от
     * удаления, а по роли строки их не отличить — обе Removed. */
    const std::vector<coder::diff::DiffChangeBlock> blocks =
        coder::diff::diff_change_blocks(rows);

    ImGui::Checkbox("Перенос строк##diff_wrap", &s_diff_word_wrap);
    ImGui::SameLine();
    if (trimmed > 0) {
        ImGui::TextDisabled("срезано общих отступов: %zu", trimmed);
    } else {
        ImGui::TextDisabled("общих отступов нет");
    }

    if (!s_diff_word_wrap) {
        const float char_w = ImGui::CalcTextSize("0").x;
        const size_t cols = coder::diff::diff_content_width(rows, gutter);
        ImGui::SetNextWindowContentSize(
            ImVec2(static_cast<float>(cols + 1) * char_w, 0.0f));
    }
    ImGui::BeginChild("##diff_rows", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    /* 0.0f — перенос по краю окна; отрицательное — не переносить вовсе. */
    ImGui::PushTextWrapPos(s_diff_word_wrap ? 0.0f : -1.0f);
    size_t block = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        const coder::diff::DiffRow& row = rows[i];
        /* Подпись блока — один раз, на его первой строке. */
        if (block < blocks.size() && blocks[block].first == i) {
            ImGui::TextColored(ImVec4(0.62f, 0.62f, 0.68f, 1.0f), "%s",
                               coder::diff::diff_change_label(blocks[block])
                                   .c_str());
            ++block;
        }
        const coder::diff::DiffColor c = coder::diff::diff_role_color(row.role);
        /* Полоса подсветки «до/после» — ПОД текстом, а не вместо него:
         * текст остаётся тем же цветом, что в 11.1, а полоса показывает,
         * к какой стороне правки строка относится. Рисуется ДО текста, иначе
         * полупрозрачная заливка легла бы поверх букв. */
        const coder::diff::DiffColor band = coder::diff::diff_change_color(
            coder::diff::diff_row_change(blocks, i), row.role);
        if (band.a > 0.0f) {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            const float pad = ImGui::GetStyle().FramePadding.x;
            auto channel = [](float v) { return static_cast<int>(v * 255.0f + 0.5f); };
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(pos.x - pad, pos.y),
                ImVec2(pos.x + width + pad,
                       pos.y + ImGui::GetTextLineHeight()),
                IM_COL32(channel(band.r), channel(band.g), channel(band.b),
                         channel(band.a)));
        }
        ImGui::TextColored(ImVec4(c.r, c.g, c.b, c.a), "%s",
                           coder::diff::format_diff_row(row, gutter).c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

static void render_snapshot_diff() {
    size_t levels = 0;
    size_t position = 0;
    {
        auto& st = engine_state();
        std::lock_guard<std::mutex> lk(st.mtx);
        levels = st.undo_stack.size();
        position = st.undo_stack.position();
    }
    /* Позиция могла уехать (агент сделал шаг), и прежний отчёт тогда
     * показывал бы чужой уровень. */
    if (s_diff_loaded && s_diff_index < levels) {
        s_diff_loaded = false;
    }
    if (levels == 0) {
        s_diff_loaded = false;
        return;
    }

    char title[96];
    std::snprintf(title, sizeof(title), "Снапшоты: уровень %zu из %zu",
                  position + 1, levels);
    if (!ImGui::TreeNodeEx(title, ImGuiTreeNodeFlags_DefaultOpen)) return;

    if (ImGui::Button("Что вернёт отмена текущего шага")) {
        s_diff_index = position;
        s_diff_report = engine().level_diff(position);
        s_diff_loaded = true;
    }
    if (!s_diff_loaded) {
        ImGui::TextDisabled("diff считается по кнопке: он читает диск, а окно"
                            " не должно делать это на каждом кадре");
        ImGui::TreePop();
        return;
    }
    if (!s_diff_report.ok) {
        ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.4f, 1.0f), "%s",
                           s_diff_report.reason.c_str());
        ImGui::TreePop();
        return;
    }
    char head[160];
    std::snprintf(head, sizeof(head), "Файлов: %zu, +%zu −%zu", s_diff_report.files.size(),
                  s_diff_report.additions(), s_diff_report.deletions());
    ImGui::Text("%s", head);
    if (s_diff_report.truncated) {
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f),
                           "Показаны не все файлы: ещё %zu (предел — показывать,"
                           " сколько влезло)", s_diff_report.omitted);
    }
    for (const coder::diff::FileDiff& fd : s_diff_report.files) {
        ImGui::PushID(fd.file.c_str());
        char line[256];
        std::snprintf(line, sizeof(line), "%s  +%zu −%zu", fd.file.c_str(),
                      fd.additions, fd.deletions);
        if (ImGui::TreeNodeEx(line, ImGuiTreeNodeFlags_DefaultOpen)) {
            if (!fd.note.empty()) {
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "%s",
                                   fd.note.c_str());
            }
            /* И11.1: настоящий виджет. Строки, номера, тематизация и
             * перенос решаются в core/diff.h; двоичный файл, изменение
             * прав и обрезанный патч приходят с пустым `patch`, и для них
             * выше сказано почему — здесь строк нет и рисовать нечего. */
            render_diff_rows(fd);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::TreePop();
}

/* --- И11.2: правки по вызовам (`ToolOutput::metadata.filediff`) ---
 *
 * Задача 11.2 — «подсветка diff до/после; diff приходит из
 * `metadata.filediff`». Данные приходят оттуда и разбираются ОДНИМ
 * вызовом `diff::filediff_from_metadata`: форма `filediff` объявлена в
 * core/diff.h и читается там же, и окно не должно знать, что там внутри
 * объекта (Д2, Д12).
 *
 * Правило блокировок — то же, что у diff уровней выше, и по той же
 * причине: `state_.mtx` берётся на КОПИЮ метаданных и отпускается, а
 * разбор патчей идёт уже без него. Разбор держит лок — то же, за что
 * закрыли D1 (окно, ждущее мьютекс агента).
 *
 * Предел — `limits::kMaxToolDiffCalls`, и пропущенное называется числом:
 * панель показывает последние вызовы, и молчаливая часть списка выглядела
 * бы как «агент правил вот столько». */
namespace {

struct ToolDiffEntry {
    std::string tool;
    std::string call_id;
    std::vector<coder::diff::FileDiff> files;
    std::string note;   /* оговорки чтения: счётчики, пропущенные записи */
};

/* Копия метаданных вызова под локом — ровно столько записей, сколько
 * панель покажет. `out` заполняется уже без лока. */
std::vector<ToolDiffEntry> collect_tool_diffs(size_t* omitted_calls) {
    std::vector<ToolDiffEntry> entries;
    size_t kept_calls = 0, kept_files = 0, dropped = 0;
    {
        auto& st = engine_state();
        std::lock_guard<std::mutex> lk(st.mtx);
        for (const Message& m : st.session) {
            for (const MessagePart& p : m.parts) {
                if (!p.is(PartKind::Tool) || !p.has_result()) continue;
                const json::JsonValue& md = p.output().metadata;
                const json::JsonValue* list =
                    md.is_object() ? md.find("filediff") : nullptr;
                if (list == nullptr || !list->is_array()) continue;
                /* Проверка предела — до копии метаданных: копировать всё
                 * содержимое сессии ради десяти показываемых блоков значило
                 * бы платить за невидимое. */
                if (kept_calls >= limits::kMaxToolDiffCalls ||
                    kept_files + list->size() > limits::kMaxDiffFiles) {
                    ++dropped;
                    continue;
                }
                ToolDiffEntry e;
                e.tool = p.tool_name();
                e.call_id = p.call_id();
                coder::json::JsonValue copied = md;
                std::vector<coder::diff::FileDiff> files;
                diff::filediff_from_metadata(copied, &files, &e.note);
                if (files.empty()) continue;
                kept_calls += 1;
                kept_files += list->size();
                e.files = std::move(files);
                entries.push_back(std::move(e));
            }
        }
    }
    *omitted_calls = dropped;
    return entries;
}

} // namespace

static void render_tool_diffs() {
    size_t omitted = 0;
    const std::vector<ToolDiffEntry> entries = collect_tool_diffs(&omitted);

    char head[96];
    std::snprintf(head, sizeof(head), "Правки по вызовам: %zu вызовов",
                  entries.size());
    if (!ImGui::TreeNodeEx(head, ImGuiTreeNodeFlags_DefaultOpen)) return;

    if (entries.empty()) {
        ImGui::TextDisabled("пишущих вызовов с diff в сессии нет");
        ImGui::TreePop();
        return;
    }
    if (omitted > 0) {
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f),
                           "Показаны последние вызовы: ещё %zu осталось за "
                           "кадром (предел — показывать, сколько влезло)",
                           omitted);
    }
    for (const ToolDiffEntry& e : entries) {
        ImGui::PushID(e.call_id.c_str());
        char line[192];
        std::snprintf(line, sizeof(line), "%s — файлов: %zu", e.tool.c_str(),
                      e.files.size());
        if (ImGui::TreeNodeEx(line, ImGuiTreeNodeFlags_DefaultOpen)) {
            if (!e.note.empty()) {
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "%s",
                                   e.note.c_str());
            }
            for (const coder::diff::FileDiff& fd : e.files) {
                ImGui::PushID(fd.file.c_str());
                char fline[320];
                std::snprintf(fline, sizeof(fline), "%s  +%zu −%zu",
                              fd.file.c_str(), fd.additions, fd.deletions);
                if (ImGui::TreeNodeEx(fline, ImGuiTreeNodeFlags_DefaultOpen)) {
                    /* Двоичный файл, изменение прав, обрезанный патч и
                     * «прав не было» приходят с пустым `patch`: строк нет, и
                     * вместо них показывается причина (граница 2 шапки
                     * core/diff.h). */
                    if (!fd.note.empty()) {
                        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f),
                                           "%s", fd.note.c_str());
                    }
                    render_diff_rows(fd);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::TreePop();
}

/* --- И11.8: диалог разрешения ---
 *
 * Решение — что показать, какие кнопки есть и что будет с остальными
 * вопросами — принимает `core/permission_panel.h`; здесь только отрисовка
 * готовых полей. Обоснование в шапке того файла.
 *
 * Снимок берётся ДО отрисовки и БЕЗ `st.mtx`: PermissionEngine живёт под
 * своим мьютексом и правилу D-6 не подчиняется — вычисления отдельно,
 * рисование отдельно. */
static void render_permission_dialog() {
    const coder::permission_panel::Panel panel =
        coder::permission_panel::build(engine().permission_pending());
    if (!panel.visible) return;

    ImGui::Separator();
    ImGui::TextUnformatted(panel.ask.headline.c_str());
    if (!panel.ask.metadata.empty()) {
        ImGui::TextWrapped("%s", panel.ask.metadata.c_str());
    }
    for (const std::string& pat : panel.ask.patterns) {
        ImGui::TextDisabled("шаблон: %s", pat.c_str());
    }
    if (!panel.ask.suggest_line.empty()) {
        ImGui::TextDisabled("%s", panel.ask.suggest_line.c_str());
    }
    /* Каскад назван ДО кнопок, а не после нажатия: «Отклонить» без этой
     * строки тихо убивает ещё N вопросов, которых человек не видел. */
    if (!panel.ask.cascade_line.empty()) {
        ImGui::TextDisabled("%s", panel.ask.cascade_line.c_str());
    }

    /* Ответ идёт по id, а не по позиции: очередь меняется между кадром и
     * кликом (каскад снимает вопросы пачкой), и ответ по позиции ушёл бы
     * не тому. */
    const std::string id = std::to_string(panel.ask.id);
    if (ImGui::SmallButton((panel.ask.once_label + "##perm" + id).c_str())) {
        engine().permission_reply(panel.ask.id, coder::PermissionReply::Once);
    }
    /* Кнопки «Всегда» нет, когда записывать нечего (пустой suggested):
     * показать её — значит пообещать постоянное разрешение, которого не
     * будет. На doom_loop она была в точности «Разрешить разово». */
    if (panel.ask.can_always) {
        ImGui::SameLine();
        if (ImGui::SmallButton((panel.ask.always_label + "##perm" + id).c_str())) {
            engine().permission_reply(panel.ask.id, coder::PermissionReply::Always);
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton((panel.ask.reject_label + "##perm" + id).c_str())) {
        engine().permission_reply(panel.ask.id, coder::PermissionReply::Reject);
    }
}

/* --- И11.3: дерево вызовов инструментов по ходам ---
 *
 * Задача — «таймлайн/дерево вызовов по ходам, сворачиваемое, с длительностью
 * и статусом каждого хода». Как в 11.1 и 11.2, решение принимает core
 * (core/timeline.h): что является узлом, какие у вызова статус и
 * длительность, как печатается время и как называется ход. Здесь остаётся
 * позвать core, покрасить и напечатать — иначе 11.13 было бы нечего
 * сверять, потому что тестового харнесса для ImGui нет.
 *
 * Правило блокировок — то же, что у панели правок выше, и по той же
 * причине: под `state_.mtx` берётся ТОЛЬКО проекция (шесть полей на вызов),
 * лок отпускается, а дерево и подписи строятся уже без него. Разбор держит
 * лок — то же, за что закрыли D1 (окно, ждущее мьютекс агента).
 *
 * Лента событий ниже остаётся: она несёт события движка, которых нет в
 * сессии (составление плана, ожидание разрешения, ошибки цикла), и таймлайн
 * их не заменяет.
 *
 * И11.4: вызов стал ВЕТКОЙ, а тело ветки рисует виджет по типу инструмента
 * (core/tool_display.h). Здесь по-прежнему только «позвать core, покрасить,
 * напечатать»: вид строки, подписи и числа решаются в core, потому что
 * 11.13 сверяет готовую строку, а собранную в окне сверять нечем.
 */

/* Раскрытые вызовы — ИДЕНТИФИКАТОРЫ, а не снимки и не тела виджетов.
 *
 * Снимок берётся отдельно и под локом (правило 3 SESSION_START, D1), а
 * список раскрытого — это то, чем окно управляет, и живёт он в UI по
 * определению. Держать здесь готовое тело значило бы завести второе место
 * правды о вызове: тело в кэше окна разошлось бы с сессией, и человек
 * увидел бы вывод, которого вызов уже не даёт. */
static std::vector<std::string> s_open_calls;

/* И11.5: режим показа деталей — переключатель ГЛОБАЛЬНЫЙ, то есть один на
 * панель, а не на вызов. Живёт в окне рядом с раскрытыми ветками и по той
 * же причине, почему живут они: это ВИД, а не свойство сессии, и в файл
 * сессии он не пишется.
 *
 * По умолчанию ВКЛЮЧЕНО (`Details::All`): 11.4 только что отдал тела
 * вызовов, и выключенный по умолчанию режим отнял бы их у того, кто
 * переключатель не трогал. Обоснование и границы режима — в шапке
 * core/timeline.h, где решение и живёт. */
static bool s_show_details = true;

static bool call_is_open(const std::string& call_id) {
    return std::find(s_open_calls.begin(), s_open_calls.end(), call_id) !=
           s_open_calls.end();
}

static void toggle_call(const std::string& call_id) {
    const auto it = std::find(s_open_calls.begin(), s_open_calls.end(), call_id);
    if (it != s_open_calls.end()) {
        s_open_calls.erase(it);
    } else {
        s_open_calls.push_back(call_id);
    }
}

/* Дерево ходов субагента: по КНОПКЕ, а не на каждом кадре.
 *
 * Тот же приём и по той же причине, что у diff уровня (10.5) и панели
 * правок: это чтение с диска, а окно не должно делать его на каждом кадре.
 * Кэш keyed по вызову и по идентификатору сессии: перечитывание при смене
 * идентификатора не даёт показать чужое дерево после «продолжить задачу»
 * (И8.9), где тот же вызов `task` уже ведёт другую сессию. */
struct ChildTree {
    std::string session;
    timeline::Timeline tl;
    std::string note;
    bool loaded = false;
};
static std::map<std::string, ChildTree> s_child_trees;

static std::string subagent_data_dir() {
    const HostCallbacks& cb = engine().callbacks();
    if (!cb.path_data_dir) return std::string();
    return cb.path_data_dir();
}

static void render_child_tree(const std::string& call_id,
                             const tool_display::Widget& w) {
    if (w.child_session.empty()) return;
    if (w.child_pending) {
        /* Фоновая постановка: работа ещё идёт, файла ещё нет. Молчаливая
         * пустота читалась бы как «у субагента не было ходов» — то есть как
         * «дерево пустое», а не как «дерева пока нет». */
        ImGui::TextDisabled("вложенное дерево: субагент ещё работает, файл "
                            "появится после");
        return;
    }
    if (!w.child_saved) {
        /* Фоновая постановка и несохранённая сессия: файла нет, и читать
         * нечего. */
        ImGui::TextDisabled("вложенное дерево: сессия субагента не записана");
        return;
    }
    ChildTree& cached = s_child_trees[call_id];
    if (cached.session != w.child_session) {
        /* Смена идентификатора сбрасывает кэш: показывать дерево прошлой
         * сессии под новым вызовом значило бы соврать о том, что делал
         * субагент. */
        cached = ChildTree();
        cached.session = w.child_session;
    }
    if (!cached.loaded) {
        if (ImGui::Button("Прочитать дерево ходов субагента##child")) {
            SessionFile file;
            std::string error;
            const std::string path = SessionArchive::subagent_file_path(
                subagent_data_dir(), w.child_session);
            /* Тот же путь, что читает resume субагента (core/subagent.cpp):
             * второй способ найти файл сессии стал бы вторым местом одного
             * факта. */
            if (SessionArchive::load(path, file, &error)) {
                cached.tl = timeline::build_timeline(timeline::timeline_source(
                    file.messages, limits::kMaxTimelineTurns));
                cached.note.clear();
            } else {
                cached.note = error;
            }
            cached.loaded = true;
        }
        ImGui::TextDisabled("чтение файла — по нажатию: окно не читает диск "
                            "на каждом кадре");
        return;
    }
    if (!cached.note.empty()) {
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "%s",
                           cached.note.c_str());
        return;
    }
    if (cached.tl.turns.empty()) {
        ImGui::TextDisabled("в файле сессии субагента ходов нет");
        return;
    }
    if (cached.tl.omitted_turns > 0) {
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f),
                           "Показаны последние ходы субагента: ещё %zu за кадром",
                           cached.tl.omitted_turns);
    }
    /* Тем же деревом, что основное (core/timeline.h): второе дерево вызовов
     * разъехалось бы с первым при первом же новом поле. */
    for (const timeline::Turn& turn : cached.tl.turns) {
        ImGui::PushID(("sub_" + turn.id).c_str());
        if (ImGui::TreeNodeEx(timeline::turn_label(turn).c_str())) {
            for (const timeline::Call& call : turn.calls) {
                const timeline::StatusColor c = timeline::status_color(call.status);
                ImGui::TextColored(ImVec4(c.r, c.g, c.b, 1.0f), "%s",
                                   timeline::call_label(call).c_str());
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

/* Тело раскрытого вызова: виджет по типу инструмента. */
static void render_call_body(const timeline::Call& call,
                             const tool_display::Snapshots& shots) {
    const tool_display::CallView* view = nullptr;
    for (const tool_display::CallView& v : shots.views) {
        if (v.call_id == call.call_id) {
            view = &v;
            break;
        }
    }
    if (view == nullptr) {
        /* Снимок берётся ДО отрисовки (лок отпускается до любого вызова
         * ImGui), поэтому тело раскрытого вызова появляется на кадр позже
         * самого раскрытия. Если же вызов попал за предел — об этом говорит
         * шапка панели, молчаливую пустоту здесь показывать нельзя. Слова
         * «раскрытых» здесь нет намеренно (11.5): при выключенных деталях
         * тело нужно и вызову, показанному целиком, а не раскрытому. */
        if (shots.omitted > 0) {
            ImGui::TextDisabled(
                "тело вызова появится на следующем кадре: %zu вызовов за кадром "
                "не показано",
                shots.omitted);
        } else {
            ImGui::TextDisabled("тело вызова появится на следующем кадре");
        }
        return;
    }

    const tool_display::Widget w = tool_display::build(*view);
    /* Подпись тела — СВОЯ строка core-формата, а не та, что в дереве: у
     * дерева она про вызов целиком (имя, состояние, время), а здесь — про то,
     * чем закончился вызов (exit, файлов, пунктов). */
    ImGui::TextUnformatted(w.head.c_str());
    if (!w.note.empty()) {
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "%s",
                           w.note.c_str());
    }
    for (const std::string& line : w.lines) {
        ImGui::TextUnformatted(line.c_str());
    }
    /* Виджет diff — УЖЕ НАПИСАННЫЙ (11.1), тот же, что у панели правок по
     * вызовам: два виджета diff рядом разъехались бы номерами строк и
     * цветами (Д2, Д12). */
    for (const coder::diff::FileDiff& fd : w.files) {
        ImGui::PushID(fd.file.c_str());
        char fline[320];
        std::snprintf(fline, sizeof(fline), "%s  +%zu −%zu", fd.file.c_str(),
                      fd.additions, fd.deletions);
        if (ImGui::TreeNodeEx(fline, ImGuiTreeNodeFlags_DefaultOpen)) {
            if (!fd.note.empty()) {
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "%s",
                                   fd.note.c_str());
            }
            render_diff_rows(fd);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (w.kind == tool_display::Kind::Task) {
        render_child_tree(call.call_id, w);
    }
}

/* --- И11.7: панель плана с чекбоксами ---
 *
 * Окно рисует ГОТОВОЕ и ничего не решает: что показано, что заблокировано
 * и какой статус получит пункт по клику — всё вычислено в
 * core/todo_panel.h.
 *
 * Единственное действие окна — клик, и оно уходит в
 * `Engine::toggle_todo`, а не правит состояние здесь. Причина в том, что
 * правка без сброса кэша промпта осталась бы для модели невидимой: план
 * печатается в системном промпте, и человек увидел бы отмеченную галочку
 * при плане, о котором модель не знает. Сброс внутри той же функции —
 * поэтому забыть его нельзя. */
static void render_todo_panel() {
    coder::todo_panel::Panel panel;
    {
        auto& st = engine_state();
        std::lock_guard<std::mutex> lk(st.mtx);
        /* Копия, а не ссылка: план переписывает инструмент `todowrite` на
         * потоке агента, и ссылка пережила бы освобождение лока (то же, за
         * что закрыли D1). */
        panel = coder::todo_panel::build(st.todos);
    }
    if (!panel.visible) return;

    if (!ImGui::TreeNodeEx("План задачи", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    ImGui::TextDisabled("%s", panel.counter_line.c_str());

    for (size_t i = 0; i < panel.items.size(); ++i) {
        const coder::todo_panel::Item& item = panel.items[i];
        ImGui::PushID(static_cast<int>(i));
        /* Идентификатор уходит в ImGui как ID, чтобы два пункта с
         * одинаковым текстом не делили одно состояние галочки. */
        ImGui::PushID(item.id.c_str());

        if (item.locked) {
            /* Заблокированный пункт рисуется БЕЗ чекбокса, а не
             * неактивным: неактивный чекбокс выглядит как «можно, но
             * нельзя», и человек тратит клик, чтобы выяснить почему. */
            ImGui::TextDisabled("%s %s (без идентификатора — правка невозможна)",
                                item.checked ? "[x]" : "[ ]", item.content.c_str());
        } else {
            bool checked = item.checked;
            /* Ключ ImGui — идентификатор пункта: без него два
             * одинаковых по тексту пункта делили бы одну галочку, и
             * отметка «прыгала» бы между ними. */
            if (ImGui::Checkbox(("##todo" + item.id).c_str(), &checked)) {
                /* Правку просит ДВИЖОК, а не окно: она обязана сбросить
                 * кэш промпта, а окно про кэш не знает. */
                engine().toggle_todo(item.id);
            }
            ImGui::SameLine();
            /* Статус подписан словами, а не только галочкой: у
             * `in_progress` и `pending` галочка одинаковая, а человек
             * должен видеть разницу. */
            ImGui::TextUnformatted(item.content.c_str());
        }
        ImGui::PopID();
        ImGui::PopID();
    }
    ImGui::TreePop();
}

static void render_timeline() {
    /* И11.5: одно решение на кадр, и решение — в core. Здесь только флаг из
     * переключателя; что из него следует — договаривается в core/timeline.h
     * (`call_show`), чтобы окно не решало «показан ли вызов» второй раз. */
    const timeline::Details details = s_show_details ? timeline::Details::All
                                                    : timeline::Details::OnlyProblems;
    timeline::Timeline tl;
    tool_display::Snapshots shots;
    {
        auto& st = engine_state();
        std::lock_guard<std::mutex> lk(st.mtx);
        /* И11.4: снимки тел — ПОД ТОТ ЖЕ ЛОК и той же строкой, что проекция
         * дерева. Копия ограничена пределом и берётся только для тех
         * вызовов, тело которых кадр покажет; разбор и подписи идут уже без
         * лока (то же, за что закрыли D1 — окно, ждущее мьютекс агента).
         *
         * И11.5: дерево и список нужных тел собираются ТОЖЕ под этим локом,
         * а не после него. Второе взятие лока ради снимков означало бы, что
         * проекция и снимки взяты из РАЗНЫХ состояний, и подпись разошлась
         * бы с картинкой. Само дерево строк не строит (`build_timeline`
         * копирует мелкие структуры, подписи зовутся при отрисовке), то
         * есть лок удлиняется на копирование, а не на разбор вывода. */
        tl = timeline::build_timeline(
            timeline::timeline_source(st.session, limits::kMaxTimelineTurns));
        shots = tool_display::snapshot_open_calls(
            st.session, timeline::body_call_ids(tl, s_open_calls, details),
            limits::kMaxToolDisplayOpen, limits::kMaxToolDisplayChars);
    }

    if (!ImGui::TreeNodeEx("Таймлайн вызовов",
                           ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    if (tl.turns.empty()) {
        ImGui::TextDisabled("ходов агента ещё не было");
        ImGui::TreePop();
        return;
    }
    /* И11.5: переключатель деталей. Подпись под ним собирает core: окно не
     * должно ни считать скрытые вызовы, ни собирать о них строку (11.13
     * сверяет готовую строку, а собранная в окне непроверяема). */
    ImGui::Checkbox("Показывать детали всех вызовов##show_details",
                    &s_show_details);
    ImGui::TextDisabled("%s", timeline::details_line(tl, details).c_str());
    ImGui::TextDisabled("%s", timeline::timeline_head(tl).c_str());
    /* Пропущенное называется числом: молча показанная часть дерева
     * выглядела бы как «агент сделал вот столько вызовов». Тот же вопрос и
     * то же правило, что у панели правок по вызовам. */
    if (tl.omitted_turns > 0) {
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f),
                           "Показаны последние ходы: ещё %zu осталось за кадром"
                           " (предел — показывать, сколько влезло)",
                           tl.omitted_turns);
    }
    /* И11.4: что панель показывает по типам. Без этой строки человек видел
     * бы серые блоки и не знал бы, что половина инструментов не описана, —
     * а это решение автора панели, а не свойство инструментов. */
    if (!shots.views.empty()) {
        ImGui::TextDisabled("%s", tool_display::panel_head(shots.views.size(),
                                                            shots.omitted).c_str());
        ImGui::TextDisabled("%s", tool_display::kinds_line().c_str());
    }
    for (const timeline::Turn& turn : tl.turns) {
        ImGui::PushID(turn.id.c_str());
        const ImGuiTreeNodeFlags flags = turn.default_open
            ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
        if (ImGui::TreeNodeEx(timeline::turn_label(turn).c_str(), flags)) {
            for (const timeline::Call& call : turn.calls) {
                ImGui::PushID(call.call_id.c_str());
                /* И11.4: вызов — ветка, и её тело рисует виджет по типу
                 * инструмента. У 11.3 он был строкой намеренно: пустая ветка
                 * тогда показала бы стрелку, которая ничего не раскрывает.
                 *
                 * И11.5: как именно показан вызов, решает core — тремя
                 * способами, и окно их только рисует:
                 *   Row    — одна строка, стрелки нет: успешный вызов при
                 *            выключенных деталях;
                 *   Branch — как в 11.4, стрелка и тело по раскрытию;
                 *   Body   — строка и тело всегда на экране: неуспешный
                 *            вызов, который иначе спрятался бы под
                 *            раскрытием (шапка core/timeline.h). */
                const timeline::CallShow show = timeline::call_show(call, details);
                const timeline::StatusColor c =
                    timeline::status_color(call.status);
                if (show == timeline::CallShow::Row) {
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImVec4(c.r, c.g, c.b, 1.0f));
                    ImGui::TextUnformatted(timeline::call_label(call).c_str());
                    ImGui::PopStyleColor();
                } else if (show == timeline::CallShow::Body) {
                    /* Отступ — тот же, что у содержимого ветки: показанный
                     * целиком вызов должен читаться как находящийся внутри
                     * хода, а не как соседняя строка того же уровня. */
                    ImGui::Indent();
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImVec4(c.r, c.g, c.b, 1.0f));
                    ImGui::TextUnformatted(timeline::call_label(call).c_str());
                    ImGui::PopStyleColor();
                    render_call_body(call, shots);
                    ImGui::Unindent();
                } else {
                    const bool open = call_is_open(call.call_id);
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImVec4(c.r, c.g, c.b, 1.0f));
                    const bool shown = ImGui::TreeNodeEx(
                        timeline::call_label(call).c_str(),
                        open ? ImGuiTreeNodeFlags_DefaultOpen
                             : ImGuiTreeNodeFlags_None);
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemToggledOpen()) toggle_call(call.call_id);
                    if (shown) {
                        render_call_body(call, shots);
                        ImGui::TreePop();
                    }
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::TreePop();
}

/* --- И11.6: панель стриминга ---
 *
 * Окно рисует ГОТОВОЕ и ничего не решает: что видно, что скрыто, какой
 * инструмент активен — всё вычислено в core/stream_panel.h. Здесь нет ни
 * одного условия «показывать ли», потому что окно ImGui юнит-тестом не
 * проверяется, и решение здесь было бы непроверяемым (тот же довод, что
 * у 11.1, 11.4, 11.5).
 *
 * Два блока состояния снимаются ОДНИМ взятием `st.mtx`: буфер и активный
 * инструмент. Второе взятие лока дало бы панель, составленную из двух
 * разных моментов времени — например, текст шага и инструмент уже
 * следующего. */
static void render_stream_panel() {
    coder::stream_panel::Panel panel;
    {
        auto& st = engine_state();
        std::lock_guard<std::mutex> lk(st.mtx);
        /* Активный инструмент берётся из ИСТОРИИ, а не из отдельного поля
         * состояния: вызов, помеченный `set_running()`, уже лежит в
         * сессии, и второе место правды «что работает» разъехалось бы при
         * первой же правке (шапка stream_panel.h, п. 3). */
        coder::stream_panel::ActiveTool active;
        for (auto it = st.session.rbegin(); it != st.session.rend(); ++it) {
            for (const coder::MessagePart& part : it->parts) {
                if (part.kind() != coder::PartKind::Tool) continue;
                if (part.state() != coder::ToolState::Running) continue;
                active.call_id = part.call_id();
                active.tool_name = part.tool_name();
                if (part.has_duration()) {
                    active.elapsed_ms = static_cast<std::size_t>(
                        part.duration_ms() < 0 ? 0 : part.duration_ms());
                }
                break;
            }
            if (!active.tool_name.empty()) break;
        }
        panel = coder::stream_panel::build(st.stream, active);
    }

    if (!panel.visible) return;

    if (!ImGui::TreeNodeEx("Стриминг", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    /* Подпись шага — из core, не собирается здесь. */
    if (!panel.step_line.empty()) {
        ImGui::TextDisabled("%s", panel.step_line.c_str());
    }

    /* Размышление идёт ПЕРВЫМ и по времени появления, а не по важности:
     * модель печатает его раньше ответа, и человек ждёт именно его. */
    if (!panel.reasoning.empty()) {
        ImGui::TextDisabled("размышление");
        ImGui::TextWrapped("%s%s", panel.reasoning.c_str(),
                           panel.reasoning_open ? "▌" : "");
        if (panel.reasoning_open) ImGui::SameLine();
        ImGui::Spacing();
    }

    /* Инструмент показывается и когда текста нет: ход, в котором модель
     * молча зовёт `bash`, не печатает ничего, и без этой строки человек
     * видел бы пустую панель при работающем инструменте. */
    if (panel.has_tool) {
        if (panel.tool.elapsed_ms > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f),
                               "%s... (%.1f с)", panel.tool.tool_name.c_str(),
                               panel.tool.elapsed_ms / 1000.0);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f),
                               "%s...", panel.tool.tool_name.c_str());
        }
    }

    /* Пишет аргументы вызова: имя уже есть, а блок ещё не разобран —
     * показывать нечего, и без этой строки человек видел бы паузу без
     * причины. */
    if (panel.tool_open) {
        ImGui::TextDisabled("пишет аргументы вызова %s",
                            panel.tool_call_id.c_str());
    }

    if (!panel.text.empty()) {
        ImGui::TextWrapped("%s%s", panel.text.c_str(),
                           panel.text_open ? "▌" : "");
    }

    /* Отброшенный текст назван ЧИСЛОМ: молча обрезанный текст выглядел бы
     * как «модель так начала», и человек искал бы причину не там. */
    if (panel.dropped_chars > 0) {
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f),
                           "начало показа скрыто: предел панели, отброшено %zu символов",
                           panel.dropped_chars);
    }

    ImGui::TreePop();
}

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
        /* И9.7: активный профиль harness. Показывается, потому что профиль
         * СУЖАЕТ права, и человек, не видя его имени, не может понять,
         * почему агент вдруг не может записать файл. Плюс причина, если
         * профиль назван, но не применился: молча неприменённый профиль
         * выглядел бы как «настройка не работает». */
        {
            coder::harness::Profile p;
            std::string perror_text;
            const bool on = engine().session_profile(&p, &perror_text);
            if (on) {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.6f, 1.0f),
                                   "Профиль: %s", p.name.c_str());
            } else if (!perror_text.empty()) {
                ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.4f, 1.0f),
                                   "Профиль не применён: %s", perror_text.c_str());
            }
        }
        if (st.state != AgentState::Idle && ImGui::Button("Стоп", {-1, 0})) {
            engine().request_abort();
        }
        ImGui::Separator();
    }

    render_snapshot_diff();
    render_tool_diffs();
    /* И11.6: панель стриминга идёт ПЕРЕД таймлайном. Пока ход открыт,
     * человек смотрит на живой текст; таймлайн под ним — уже закрытые
     * ходы. Наоборот (таймлайн выше) панель оказалась бы внизу экрана
     * именно тогда, когда в неё смотрят. */
    render_stream_panel();
    render_todo_panel();
    render_timeline();

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

    /* Размер сессии (И7.2).
     *
     * Показываются ТОКЕНЫ и источник числа, а не символы: сжимать
     * контекст по символам — значит угадывать (chars/4 — оценка, и у
     * кириллицы токен короче, у кода длиннее). Источник назван прямо,
     * потому что «оценка» и «измерено провайдером» выглядели бы
     * одинаково, а значимость у них разная.
     *
     * Порог показывается, только если он известен: без лимимов модели
     * (их берутся из настроек — в ABI хоста их нет) выдуманное число
     * выглядело бы как обещание, и «не влезает» пришлось бы ловить по
     * факту отказа провайдера. Пока порога нет, подпись говорит об
     * этом словами. */
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        if (!st.session.empty()) {
            const compaction::ContextUsage used = compaction::context_usage(
                st.measured_input_tokens, st.session);
            const long long room =
                compaction::usable(st.model_limits, st.compaction_config);
            ImGui::SameLine();
            if (room > 0) {
                ImGui::TextDisabled("ctx: %lld ток / %lld (%s)",
                                    used.tokens, room,
                                    compaction::token_source_name(used.source));
            } else {
                ImGui::TextDisabled("ctx: %lld ток (%s, лимит не задан)",
                                    used.tokens,
                                    compaction::token_source_name(used.source));
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Сколько токенов занял последний запрос к модели.\n"
                    "«измерено» — число от провайдера, «оценка» — chars/4.\n"
                    "Лимит окна берётся из настроек плагина:\n"
                    "wp_coder.context_limit, wp_coder.input_limit,\n"
                    "wp_coder.max_output_tokens, wp_coder.compaction_auto,\n"
                    "wp_coder.compaction_reserved.");
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

    /* Запросы системы разрешений И2 рисует render_permission_dialog —
     * решение о подписях и кнопках принимает core/permission_panel.h. */
    render_permission_dialog();

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
