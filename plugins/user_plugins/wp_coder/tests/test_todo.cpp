/*
 * test_todo.cpp — И4.6: план задачи (todowrite / toread).
 *
 * План проверяется по трём свойствам, и все три — про «враньё в
 * интерфейсе», а не про наличие полей:
 *   1. План попадает в СИСТЕМНЫЙ промпт (иначе модель его не видит и
 *      список мёртвый) и обновляется сразу, а не со второго шага.
 *   2. Нераспознанный статус/приоритет не превращается молча в дефолт:
 *      подмена объявляется в ответе.
 *   3. План новой задачи не наследуется от прошлой.
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/json.h"
#include "../core/tool.h"
#include "../core/tools_registry.h"
#include "../core/engine.h"
#include "../core/base_tools.h"
#include "../core/prompts.h"

#include <string>
#include <vector>

using namespace coder;

namespace {

void init_tools() {
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ChatMsg>&, LlmReply&) { return false; };
    cb.llm_complete = [](const std::string&, const std::string&, std::string&) { return false; };
    cb.llm_is_connected = []() { return false; };
    cb.chat_event = [](const std::string&) {};
    Engine::instance().init(cb);
    test_support::approve_all_permissions();
    register_base_tools();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().todos.clear();
        engine_state().prompt_dirty = true;
    }
}

json::JsonValue todo(const char* id, const char* content, const char* status,
                     const char* priority = "medium") {
    json::JsonValue t = json::JsonValue::object();
    t.set("id", id);
    t.set("content", content);
    t.set("status", status);
    t.set("priority", priority);
    return t;
}

std::string run_todowrite(const std::vector<json::JsonValue>& items) {
    json::JsonValue list = json::JsonValue::array();
    for (const auto& i : items) list.push_back(i);
    json::JsonValue args = json::JsonValue::object();
    args.set("todos", std::move(list));
    return ToolsRegistry::instance().run("todowrite", args);
}

std::string system_prompt() { return engine().build_system_prompt(); }

} // anonymous namespace

/* ======================================================================
 * Инструменты
 * ====================================================================== */

TEST(todo_tools_are_registered_and_read_only) {
    init_tools();
    const ToolDef* w = ToolsRegistry::instance().find("todowrite");
    const ToolDef* r = ToolsRegistry::instance().find("todoread");
    ASSERT_TRUE(w != nullptr);
    ASSERT_TRUE(r != nullptr);
    /* План не касается файлов: в режиме Research он должен быть доступен. */
    ASSERT_TRUE(tf_has(w->flags, TF_READ_ONLY));
    ASSERT_TRUE(tf_has(r->flags, TF_READ_ONLY));
    ASSERT_EQ(permission_key_of(*w), std::string("todo"));
    std::string cat = ToolsRegistry::instance().build_tool_catalogue();
    ASSERT_TRUE(cat.find("- todowrite —") != std::string::npos);
    /* В описании массива модель должна видеть форму элемента. */
    ASSERT_TRUE(cat.find("todos: список — массив пунктов") != std::string::npos);
}

TEST(todowrite_requires_the_array) {
    init_tools();
    json::JsonValue empty = json::JsonValue::object();
    std::string r = ToolsRegistry::instance().run("todowrite", empty);
    ASSERT_TRUE(r.find("invalid arguments") != std::string::npos);
    ASSERT_TRUE(r.find("todos") != std::string::npos);

    /* Массив обязателен: одиночная строка — самая частая ошибка модели,
     * и её отсекает валидация схемы, ДО входа в обработчик. */
    json::JsonValue wrong = json::JsonValue::object();
    wrong.set("todos", "осмотреть проект");
    std::string r2 = ToolsRegistry::instance().run("todowrite", wrong);
    ASSERT_TRUE(r2.find("must be array") != std::string::npos);
    ASSERT_TRUE(r2.find("todos") != std::string::npos);
}

TEST(todowrite_rejects_items_without_content) {
    init_tools();
    std::string r = run_todowrite({todo("1", "", "pending")});
    ASSERT_TRUE(r.find("пустой content") != std::string::npos);
    /* Список не принят целиком: молча выброшенный пункт выглядел бы как
     * «пунктов нет». */
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        ASSERT_TRUE(engine_state().todos.empty());
    }
}

TEST(todowrite_assigns_ids_when_the_model_omits_them) {
    init_tools();
    /* Ссылаться на пункт по id модель должна уметь всегда, а id пишет
     * через раз. */
    std::string r = run_todowrite({todo("", "осмотреть", "in_progress"),
                                   todo("", "починить", "pending")});
    ASSERT_TRUE(r.find("[in_progress] 1. осмотреть") != std::string::npos);
    ASSERT_TRUE(r.find("[pending] 2. починить") != std::string::npos);
}

TEST(todowrite_normalizes_statuses_and_says_so) {
    init_tools();
    std::string r = run_todowrite({todo("1", "сделать", "готово"),
                                    todo("2", "проверить", "ВЫПОЛНЕНО"),
                                    todo("3", "ещё", "выдумка")});
    ASSERT_TRUE(r.find("[completed] 1. сделать") != std::string::npos);
    /* ПРОПИСНЫМИ: tolower в локали «C» не трогает кириллицу, и список
     * молча рассыпался бы. */
    ASSERT_TRUE(r.find("[completed] 2. проверить") != std::string::npos);
    /* Неизвестное значение не проходит молча: иначе «план исполнен» был бы
     * виден пользователю, а модель имела в виду другое. */
    ASSERT_TRUE(r.find("нормализовано") != std::string::npos);
    ASSERT_TRUE(r.find("выдумка") != std::string::npos);
    ASSERT_TRUE(r.find("взят pending") != std::string::npos);
}

TEST(todowrite_rejects_empty_list) {
    init_tools();
    std::string r = run_todowrite({});
    ASSERT_TRUE(r.find("пустой список") != std::string::npos);
}

TEST(todoread_shows_the_current_plan) {
    init_tools();
    run_todowrite({todo("1", "осмотреть", "in_progress", "high")});
    std::string r = ToolsRegistry::instance().run("todoread", json::JsonValue::object());
    ASSERT_TRUE(r.find("1. осмотреть") != std::string::npos);
    ASSERT_TRUE(r.find("(high)") != std::string::npos);
}

/* ======================================================================
 * План в системном промпте
 * ====================================================================== */

TEST(todo_plan_reaches_the_system_prompt) {
    init_tools();
    ASSERT_TRUE(system_prompt().find("ПЛАН ЗАДАЧИ") == std::string::npos);
    run_todowrite({todo("1", "осмотреть проект", "in_progress"),
                   todo("2", "починить баг", "pending")});
    std::string p = system_prompt();
    ASSERT_TRUE(p.find("## ПЛАН ЗАДАЧИ (todowrite)") != std::string::npos);
    ASSERT_TRUE(p.find("осмотреть проект") != std::string::npos);
    ASSERT_TRUE(p.find("починить баг") != std::string::npos);
    /* Модель должна знать, что план ей показывают и что его надо вести. */
    ASSERT_TRUE(p.find("in_progress") != std::string::npos);
}

TEST(todo_plan_refreshes_immediately_after_change) {
    init_tools();
    run_todowrite({todo("1", "осмотреть", "pending")});
    ASSERT_TRUE(system_prompt().find("осмотреть") != std::string::npos);
    /* Смена статуса обязана быть видна на СЛЕДУЮЩЕМ шаге: кэш системного
     * промпта без сброса показывал бы старый список ещё весь ход. */
    run_todowrite({todo("1", "осмотреть", "completed")});
    std::string p = system_prompt();
    ASSERT_TRUE(p.find("[completed] осмотреть") != std::string::npos);
    ASSERT_TRUE(p.find("[pending] осмотреть") == std::string::npos);
}

TEST(todo_plan_is_cleared_with_the_session) {
    init_tools();
    run_todowrite({todo("1", "прошлая задача", "pending")});
    engine().clear_session();
    /* Чужой план в промпте новой задачи только сбивает. */
    ASSERT_TRUE(system_prompt().find("прошлая задача") == std::string::npos);
}
