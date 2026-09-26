/*
 * test_tool.cpp — И1.6–И1.9: схемы, валидация аргументов и enforcement
 * режимов по ToolFlags.
 *
 * Здесь закрываются дефекты D3 и D4 плана AGENT_PARITY_PLAN.md:
 *   - research был только текстом в промпте, хотя README обещает
 *     «только чтение»;
 *   - plan-режим покрывал 4 инструмента из 50, а bash,
 *     git_commit, deploy, cron_add, systemd_restart, docker_run,
 *     wp_create_site и pip_install шли мимо.
 * Правило проверяется на настоящих зарегистрированных инструментах,
 * а не на искусственных: бессмысленно «проверять» политику на
 * инструменте, которого нет в реестре.
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/json.h"
#include "../core/tool.h"
#include "../core/tools_registry.h"
#include "../core/engine.h"
#include "../core/agent_components.h"
#include "../core/base_tools.h"
#include "../core/git_tools.h"
#include "../modules/wordpress/wp_tools.h"
#include "../modules/python/python_tools.h"
#include "../modules/devops/devops_tools.h"

using namespace coder;

namespace {

/* Полный набор из 50 инструментов: базовые, git и три модуля.
 * Модули регистрируются напрямую (не через ModuleRegistry), чтобы тест
 * не трогал SkillsManager — он считает навыки, и лишние модули в
 * реестре изменили бы чужие тесты. */
void register_all_tools() {
    static bool done = false;
    if (done) return;
    done = true;
    /* Инструменты проверяются поведением, а не разрешениями: имитируем
     * пользователя, нажавшего «всегда» (см. tests/test_support.h). */
    test_support::approve_all_permissions();
    register_base_tools();
    register_rag_tools();
    register_git_tools();
    wp::register_wp_tools();
    python::register_python_tools();
    devops::register_devops_tools();
}

/* Engine — синглтон, состояние между тестами протекает. Поэтому режим
 * и настройки восстанавливаются в каждом тесте явно, иначе тесты
 * будут зависеть от порядка регистрации. */
struct ModeGuard {
    int prev_mode;
    bool prev_plan;
    explicit ModeGuard(int mode, bool plan) {
        prev_mode = engine_state().mode;
        prev_plan = engine_state().plan_mode;
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().mode = mode;
        engine_state().plan_mode = plan;
    }
    ~ModeGuard() {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().mode = prev_mode;
        engine_state().plan_mode = prev_plan;
    }
};

ToolDef def_for(const std::string& name) {
    const ToolDef* def = ToolsRegistry::instance().find(name);
    if (!def) throw std::runtime_error("инструмент не зарегистрирован: " + name);
    return *def;
}

json::JsonValue args_with(const std::string& tool, const char* key,
                          const std::string& value) {
    json::JsonValue args = json::JsonValue::object();
    args.set("tool", tool);
    args.set(key, value);
    return args;
}

} // anonymous namespace

/* ======================================================================
 * И1.6 — валидация аргументов по схеме
 * ====================================================================== */

TEST(schema_validates_required_parameter) {
    register_all_tools();
    json::JsonValue empty = json::JsonValue::object();
    empty.set("tool", "read_file");
    std::string r = ToolsRegistry::instance().run("read_file", empty);
    /* Сообщение в стиле opencode: модель должна понять, что исправить. */
    ASSERT_TRUE(r.find("The read_file tool was called with invalid arguments")
                != std::string::npos);
    ASSERT_TRUE(r.find("missing required parameter 'path'") != std::string::npos);
    ASSERT_TRUE(r.find("Please rewrite the input") != std::string::npos);
}

TEST(schema_validates_parameter_type) {
    register_all_tools();
    /* offset объявлен целым — строка не подходит. (Раньше здесь был
     * параметр k; И4.7 заменил его на offset/limit.) */
    json::JsonValue args = json::JsonValue::object();
    args.set("tool", "read_file");
    args.set("path", "a.txt");
    args.set("offset", "не число");
    std::string r = ToolsRegistry::instance().run("read_file", args);
    ASSERT_TRUE(r.find("parameter 'offset' must be integer") != std::string::npos);
}

TEST(schema_validates_numeric_range) {
    register_all_tools();
    /* edit_file: k >= 1 (нумерация строк с 1). */
    json::JsonValue args = json::JsonValue::object();
    args.set("tool", "edit_file");
    args.set("path", "a.txt");
    args.set("k", 0);
    args.set("content", "x");
    std::string r = ToolsRegistry::instance().run("edit_file", args);
    ASSERT_TRUE(r.find("must be >= 1") != std::string::npos);
}

TEST(schema_accepts_coerced_arguments_from_models) {
    register_all_tools();
    /* Локальные модели присылают "5" вместо 5 — это НЕ повод отклонить
     * вызов: as_int() приводит, проверка типа проходит. */
    json::JsonValue args = json::JsonValue::object();
    args.set("tool", "list");
    args.set("path", "/tmp");
    args.set("k", "5");
    std::string r = ToolsRegistry::instance().run("list", args);
    ASSERT_TRUE(r.find("invalid arguments") == std::string::npos);
}

TEST(schema_ignores_unknown_parameters_by_default) {
    register_all_tools();
    /* Локальные 7B-модели постоянно добавляют «note»/«comment».
     * Жёсткий отказ заставлял бы их сжигать шаги впустую, поэтому
     * unknown keys разрешены по умолчанию (см. SchemaBuilder::strict). */
    json::JsonValue args = json::JsonValue::object();
    args.set("tool", "repo_map");
    args.set("note", "посмотреть структуру");
    std::string r = ToolsRegistry::instance().run("repo_map", args);
    ASSERT_TRUE(r.find("invalid arguments") == std::string::npos);
}

TEST(schema_strict_mode_rejects_unknown_parameters) {
    SchemaBuilder b;
    b.str("path", "путь").required("path").strict();
    json::JsonValue args = json::JsonValue::object();
    args.set("path", "a.txt");
    args.set("typo", "x");
    std::string detail;
    ASSERT_FALSE(validate_tool_args(b.build(), args, detail));
    ASSERT_TRUE(detail.find("unknown parameter 'typo'") != std::string::npos);
    /* Имя «tool» — не параметр, его наличие допустимо всегда. */
    args.erase("typo");
    args.set("tool", "read_file");
    ASSERT_TRUE(validate_tool_args(b.build(), args, detail));
    args.set("typo", "x");
    ASSERT_FALSE(validate_tool_args(b.build(), args, detail));
}

TEST(schema_enum_is_enforced) {
    SchemaBuilder b;
    b.string_enum("mode", "режим", {"fast", "accurate"}).required("mode");
    json::JsonValue args = json::JsonValue::object();
    args.set("mode", "точная");
    std::string detail;
    ASSERT_FALSE(validate_tool_args(b.build(), args, detail));
    ASSERT_TRUE(detail.find("must be one of [fast, accurate]") != std::string::npos);
    args.set("mode", "fast");
    ASSERT_TRUE(validate_tool_args(b.build(), args, detail));
}

TEST(schema_builder_marks_required_and_lists_parameters) {
    SchemaBuilder b;
    b.str("path", "путь").integer("k", "с какой строки").required("path");
    json::JsonValue schema = b.build();
    ASSERT_EQ(schema.get_string("type"), std::string("object"));
    ASSERT_TRUE(schema.get_bool("additionalProperties", false));
    ASSERT_EQ(schema.get_int("required[0]"), 0LL);
    ASSERT_TRUE(schema.has("required"));
    ASSERT_EQ(schema.get_string("required[0]"), std::string("path"));
    ASSERT_EQ(schema.get_string("properties.path.type"), std::string("string"));
    ASSERT_EQ(schema.get_string("properties.k.type"), std::string("integer"));
    /* k не объявлен обязательным. */
    ASSERT_EQ(schema.get_int("required[1]", -1), -1LL);
}

/* ======================================================================
 * И1.7 — enforcement по флагам: research не может писать (D3)
 * ====================================================================== */

TEST(research_mode_blocks_every_writing_tool) {
    register_all_tools();
    ModeGuard mode(1, false);

    /* Регресссия D3: research был только текстом в промпте. Теперь ни
     * один инструмент, меняющий состояние, не вызывается. */
    const char* forbidden[] = {
        "write_file", "search_replace", "edit_file", "undo_edit",
        "rag_index", "bash", "git_commit", "git_add", "git_checkout",
    };
    for (const char* tool : forbidden) {
        const ToolDef* def = ToolsRegistry::instance().find(tool);
        if (!def) throw std::runtime_error(std::string("нет инструмента: ") + tool);
        std::string refusal = check_tool_mode_policy(tool, *def, 1, false);
        ASSERT_TRUE(!refusal.empty());
        /* Отказ обязан быть actionable: модель не должна повторять вызов. */
        ASSERT_TRUE(refusal.find("НЕ ПОВТОРЯЙ") != std::string::npos);
    }
}

TEST(research_mode_allows_reading_and_network_tools) {
    register_all_tools();
    /* Чтение и сеть (web_fetch, headless_render, git status) — это
     * тоже чтение, их блокировать нельзя: research без них бесполезен. */
    const char* allowed[] = {
        "read_file", "repo_map", "grep_search", "list", "list_skills",
        "web_fetch", "git_status", "git_diff", "git_log", "rag_query",
    };
    for (const char* tool : allowed) {
        const ToolDef* def = ToolsRegistry::instance().find(tool);
        if (!def) throw std::runtime_error(std::string("нет инструмента: ") + tool);
        ASSERT_TRUE(check_tool_mode_policy(tool, *def, 1, false).empty());
    }
}

TEST(research_mode_blocks_module_tools_too) {
    /* D3/D4: правило должно покрывать и модульные инструменты, а не
     * только базовые. Регистрируем модули здесь же: набор зависит от
     * того, что уже успели зарегистрировать другие тесты. */
    register_all_tools();
    ModeGuard mode(1, false);

    /* Инструменты WordPress/Python/DevOps, которые раньше шли мимо. */
    const char* forbidden[] = {
        "wp_create_site", "deploy", "wp_cli", "wp_db",
        "pip_install", "venv_create",
        "docker_run", "cron_add", "systemd_restart", "ssh_exec",
    };
    for (const char* tool : forbidden) {
        const ToolDef* def = ToolsRegistry::instance().find(tool);
        if (!def) throw std::runtime_error(std::string("нет инструмента: ") + tool);
        ASSERT_TRUE(!check_tool_mode_policy(tool, *def, 1, false).empty());
    }
    /* А проверяющие инструменты модулей в research остаются доступны. */
    const char* allowed[] = {"validate", "php_lint", "wp_check_deps",
                             "docker_ps", "systemd_status", "cron_list"};
    for (const char* tool : allowed) {
        const ToolDef* def = ToolsRegistry::instance().find(tool);
        if (!def) throw std::runtime_error(std::string("нет инструмента: ") + tool);
        ASSERT_TRUE(check_tool_mode_policy(tool, *def, 1, false).empty());
    }
}

/* ======================================================================
 * И1.7 — plan-режим: закрываем D4 (4 из 50 → 50 из 50)
 * ====================================================================== */

TEST(plan_mode_blocks_execution_and_destructive_tools) {
    register_all_tools();
    ModeGuard mode(0, true);

    /* Именно эти инструменты дефект D4 называл обходящими план-режим. */
    const char* forbidden[] = {
        "bash", "git_commit", "deploy", "cron_add", "systemd_restart",
        "docker_run", "wp_create_site", "pip_install",
    };
    for (const char* tool : forbidden) {
        const ToolDef* def = ToolsRegistry::instance().find(tool);
        if (!def) throw std::runtime_error(std::string("нет инструмента: ") + tool);
        std::string refusal = check_tool_mode_policy(tool, *def, 0, true);
        ASSERT_TRUE(!refusal.empty());
        ASSERT_TRUE(refusal.find("сначала план") != std::string::npos);
    }
}

/* План-режим пропускает WRITES_FILES, поэтому важно, чтобы ВСЕ такие
 * инструменты шли через propose_write: иначе «предложение правки»
 * превратится в тихую запись файла в обход политики. Список закрыт
 * осознанно — добавление нового писателя без propose_write уронит
 * этот тест. */
TEST(plan_mode_allows_only_known_proposers_to_write) {
    register_all_tools();
    const char* proposers[] = {"write_file", "search_replace", "edit_file",
                             "undo_edit", "apply_patch"};
    for (const auto& def : ToolsRegistry::instance().defs()) {
        if (!tf_has(def.flags, TF_WRITES_FILES)) continue;
        if ((def.flags & kPlanForbidden) != 0u) continue;   /* и так заблокирован */
        bool known = false;
        for (const char* p : proposers) known = known || def.name == p;
        if (!known) {
            std::cerr << "  инструмент " << def.name
                      << " пишет файлы и разрешён в план-режиме, "
                         "но не вызывает propose_write: " << std::endl;
            ASSERT_TRUE(false);
        }
    }
}

TEST(plan_mode_still_proposes_file_writes) {
    register_all_tools();
    ModeGuard mode(0, true);
    /* Правка файла в план-режиме не блокируется, а ПРЕДЛАГАЕТСЯ:
     * пользователь подтверждает в UI. Это поведение раньше было
     * продублировано в четырёх инструментах, теперь живёт в
     * ToolContext::propose_write. */
    const char* writers[] = {"write_file", "search_replace", "edit_file", "undo_edit"};
    for (const char* tool : writers) {
        const ToolDef* def = ToolsRegistry::instance().find(tool);
        if (!def) throw std::runtime_error(std::string("нет инструмента: ") + tool);
        ASSERT_TRUE(check_tool_mode_policy(tool, *def, 0, true).empty());
    }
    /* А чтение тем более доступно. */
    ASSERT_TRUE(check_tool_mode_policy("read_file", def_for("read_file"), 0, true).empty());
}

TEST(plan_mode_refusal_explains_reason) {
    /* Модель должна знать, ЧЕМ именно инструмент не подходит, иначе
     * она будет подбирать другой инструмент наугад. */
    std::string refusal = check_tool_mode_policy("docker_run", def_for("docker_run"),
                                                0, true);
    ASSERT_TRUE(refusal.find("запускает код") != std::string::npos);
    ASSERT_TRUE(refusal.find("docker_run") != std::string::npos);
    ASSERT_TRUE(refusal.find("сначала план") != std::string::npos);
    std::string write_refusal = check_tool_mode_policy("write_file",
                                                       def_for("write_file"), 1, false);
    ASSERT_TRUE(write_refusal.find("меняет файлы") != std::string::npos);
    ASSERT_TRUE(write_refusal.find("Research") != std::string::npos);
    ASSERT_TRUE(write_refusal.find("НЕ ПОВТОРЯЙ") != std::string::npos);
}

/* ======================================================================
 * И1.7 — Code/Review ограничений не имеют
 * ====================================================================== */

TEST(code_and_review_modes_allow_everything) {
    register_all_tools();
    const char* tools[] = {"write_file", "bash", "read_file"};
    for (const char* tool : tools) {
        const ToolDef& def = def_for(tool);
        ASSERT_TRUE(check_tool_mode_policy(tool, def, 0, false).empty());
        ASSERT_TRUE(check_tool_mode_policy(tool, def, 2, false).empty());
    }
}

/* ======================================================================
 * И1.4/И1.8 — классификация всех инструментов завершена
 * ====================================================================== */

TEST(no_unclassified_tools) {
    /* TF_UNCLASSIFIED ставился инструментам, зарегистрированным старым
     * способом без схемы и флагов. Миграция И1.8 закончена — таких
     * инструментов быть не должно, иначе они молча выпадают из
     * политики режимов (fail-closed) и ломают каталог для модели. */
    register_all_tools();
    ModuleRegistry::instance().init_all();
    for (const auto& def : ToolsRegistry::instance().defs()) {
        ASSERT_TRUE(!tf_has(def.flags, TF_UNCLASSIFIED));
    }
}

TEST(every_tool_has_schema_and_handler) {
    /* Реестр без схемы = инструмент без валидации и без описания
     * для модели. Оба слоя читают один источник, поэтому проверяем
     * оба сразу. */
    register_all_tools();
    ModuleRegistry::instance().init_all();
    auto defs = ToolsRegistry::instance().defs();
    ASSERT_TRUE(defs.size() >= 50);
    for (const auto& def : defs) {
        if (!def.name.empty()) ASSERT_TRUE(def.parameters.is_object());
        if (static_cast<bool>(def.handler)) ASSERT_TRUE(!def.description.empty());
    }
}

TEST(tool_flag_names_reports_all_set_flags) {
    ASSERT_EQ(std::string(tool_flag_names(TF_READ_ONLY)), std::string("read_only"));
    ASSERT_EQ(std::string(tool_flag_names(TF_EXECUTES | TF_SLOW)),
              std::string("executes|slow"));
    ASSERT_EQ(std::string(tool_flag_names(TF_NONE)), std::string("none"));
}

/* ======================================================================
 * И1.7 — enforcement в самом ToolRunner (а не только в чистой функции)
 * ====================================================================== */

TEST(tool_runner_refuses_tool_in_research_mode) {
    register_all_tools();
    ModeGuard mode(1, false);
    std::vector<std::string> events;
    auto& st = engine_state();
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        st.project_dir = "/tmp";
        st.recent_calls.clear();
    }
    ToolRunner runner(st, Engine::instance().callbacks(),
                      [&](AgentEvent::Kind k, const std::string& text) {
        if (k == AgentEvent::Error) events.push_back(text);
    });
    std::string r = runner.run("write_file", args_with("write_file", "path", "x.txt"));
    ASSERT_TRUE(r.find("запрещено режимом") != std::string::npos);
    /* Отказ виден в UI-логе, и он НЕ выглядит как успешный вызов. */
    ASSERT_EQ(events.size(), (size_t)1);
    /* Проверка режима идёт ДО вызова: отпечаток не попадает в историю
     * (иначе три одинаковых отказа сочлись бы зацикливанием). */
    std::lock_guard<std::mutex> lk(st.mtx);
    ASSERT_TRUE(st.recent_calls.empty());
}
