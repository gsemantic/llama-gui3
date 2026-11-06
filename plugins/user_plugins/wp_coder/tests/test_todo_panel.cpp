/*
 * test_todo_panel.cpp — панель плана задачи (И11.7).
 *
 * Проверяется не «чекбокс нарисован», а то, что человек получит от
 * панели и что модель получит от его клика: счётчик незавершённых,
 * переключение статуса и — главное — что правка по индексу изменила бы
 * не тот пункт, а потому ищется по идентификатору.
 */

#include "core/todo_panel.h"

#include "core/engine.h"
#include "test_framework.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using namespace coder;
using namespace coder::todo_panel;

namespace {

TodoItem todo(const char* id, const char* status) {
    TodoItem t;
    t.id = id;
    t.content = std::string("пункт ") + id;
    t.status = status;
    return t;
}

/* План из трёх пунктов: два в работе, один выполнен. */
std::vector<TodoItem> sample_plan() {
    return {todo("1", "pending"), todo("2", "in_progress"),
            todo("3", "completed")};
}

const Item* find_item(const Panel& p, const std::string& id) {
    for (const Item& it : p.items) {
        if (it.id == id) return &it;
    }
    return nullptr;
}

}  // namespace

/* --- Видимость --- */

TEST(пустой_план_не_показывает_панель) {
    /* Рамка с заголовком и нулём выглядела бы как «план сломался». */
    const Panel p = build(std::vector<TodoItem>());
    ASSERT_FALSE(p.visible);
    ASSERT_TRUE(p.items.empty());
}

TEST(непустой_план_показывает_панель) {
    ASSERT_TRUE(build(sample_plan()).visible);
}

/* --- Счётчик незавершённых --- */

TEST(счётчик_считает_незавершённые_а_не_все) {
    /* Ровно за этим человек и смотрит: «сколько ещё делать». */
    const Panel p = build(sample_plan());
    ASSERT_EQ(std::to_string(p.remaining), std::to_string(2));
    ASSERT_EQ(std::to_string(p.total), std::to_string(3));
}

TEST(отменённый_пункт_не_считается_незавершённым) {
    /* Отменённый никто уже не выполнит, и «осталось 2 из 3», где третий
     * отменён, вводит в заблуждение ровно на том числе, ради которого
     * сюда смотрят. */
    std::vector<TodoItem> plan = {todo("1", "pending"), todo("2", "cancelled"),
                                  todo("3", "completed")};
    const Panel p = build(plan);
    ASSERT_EQ(std::to_string(p.remaining), std::to_string(1));
    ASSERT_EQ(std::to_string(p.total), std::to_string(3));
}

TEST(подпись_счётчика_называет_незавершённых) {
    const Panel p = build(sample_plan());
    ASSERT_TRUE(p.counter_line.find("Незавершённых: 2 из 3") != std::string::npos);
}

TEST(подпись_пустого_состояния_объясняет_отсутствие) {
    /* «ходов ещё не было» после первого хода читается как «панель
     * сломалась» — человек начинает искать несуществующую проблему. */
    const std::string line = empty_line();
    ASSERT_FALSE(line.empty());
    ASSERT_TRUE(line.find("Плана нет") != std::string::npos);
}

/* --- Галочка по статусу --- */

TEST(галочка_по_равенству_completed) {
    const Panel p = build(sample_plan());
    ASSERT_TRUE(find_item(p, "3")->checked);
    ASSERT_FALSE(find_item(p, "1")->checked);
}

TEST(отменённый_не_выглядит_выполненным) {
    /* Иначе крестик читался бы как сделанное, а это прямо
     * противоположный смысл. */
    std::vector<TodoItem> plan = {todo("1", "cancelled")};
    const Panel p = build(plan);
    ASSERT_FALSE(p.items[0].checked);
}

/* --- Переключатель --- */

TEST(клик_закрывает_пункт) {
    ASSERT_EQ(next_status("pending"), std::string("completed"));
    ASSERT_EQ(next_status("in_progress"), std::string("completed"));
}

TEST(клик_возвращает_ошибочно_отмеченное) {
    /* Человек нажал галочку по ошибке — пункт должен вернуться в
     * работу, иначе ошибиться необратимо. */
    ASSERT_EQ(next_status("completed"), std::string("pending"));
}

/* --- Применение правки --- */

TEST(клик_меняет_план_по_идентификатору) {
    std::vector<TodoItem> plan = sample_plan();
    ASSERT_TRUE(apply_toggle(plan, "1"));
    ASSERT_EQ(plan[0].status, std::string("completed"));
    /* Остальные не тронуты: правка точечная. */
    ASSERT_EQ(plan[1].status, std::string("in_progress"));
    ASSERT_EQ(plan[2].status, std::string("completed"));
}

TEST(клик_возвращает_пункт_в_работу) {
    std::vector<TodoItem> plan = sample_plan();
    ASSERT_TRUE(apply_toggle(plan, "3"));
    ASSERT_EQ(plan[2].status, std::string("pending"));
}

TEST(несуществующий_идентификатор_не_меняет_план) {
    /* План мог измениться между кадром и кликом. Правка по старому
     * идентификатору изменила бы НЕ ТОТ пункт — лучше не применить
     * ничего. */
    std::vector<TodoItem> plan = sample_plan();
    const std::string before = plan[0].status;
    ASSERT_FALSE(apply_toggle(plan, "99"));
    ASSERT_EQ(plan[0].status, before);
}

TEST(правка_идёт_по_идентификатору_а_не_по_позиции) {
    /* Прямая проверка мотива: перестановка пунктов не должна менять
     * то, какой из них закроется. */
    std::vector<TodoItem> plan = {todo("1", "pending"), todo("2", "pending"),
                                  todo("3", "pending")};
    std::swap(plan[0], plan[2]);
    ASSERT_TRUE(apply_toggle(plan, "1"));
    ASSERT_EQ(plan[2].status, std::string("completed"));
    ASSERT_EQ(plan[0].status, std::string("pending"));
}

/* --- Заблокированные пункты --- */

TEST(пункт_без_идентификатора_заблокирован) {
    /* Вернуть правку нечем, а нажатая галочка, которая ничего не
     * делает, хуже явного «недоступно». */
    ASSERT_TRUE(is_locked(""));
}

TEST(пункт_с_идентификатором_не_заблокирован) {
    ASSERT_FALSE(is_locked("1"));
}

TEST(заблокированный_пункт_в_панели_помечен) {
    std::vector<TodoItem> plan = {todo("", "pending")};
    const Panel p = build(plan);
    ASSERT_TRUE(p.items[0].locked);
}

TEST(заблокированный_пункт_не_меняется_кликом) {
    std::vector<TodoItem> plan = {todo("", "pending")};
    ASSERT_FALSE(apply_toggle(plan, ""));
    ASSERT_EQ(plan[0].status, std::string("pending"));
}

/* --- Неизвестный статус виден как есть --- */

TEST(неизвестный_статус_не_считается_выполненным) {
    /* Статусы приходят от модели текстом, и нераспознанное значение
     * должно быть видно как есть, а не молча стать «готово». */
    std::vector<TodoItem> plan = {todo("1", "что-то_новое")};
    const Panel p = build(plan);
    ASSERT_FALSE(p.items[0].checked);
    ASSERT_EQ(std::to_string(p.remaining), std::to_string(1));
}
/* --- Запись через движок: правка доходит до промпта --- */

TEST(клик_через_движок_меняет_план_и_сбрасывает_кэш_промпта) {
    /* Проверяется СВЯЗКА, которой больше нигде нет: план печатается в
     * системном промпте, поэтому правка без сброса кэша осталась бы для
     * модели невидимой, и человек увидел бы отмеченную галочку при
     * плане, которого модель не знает. Это ровно тот случай, где
     * «панель работает» и «панель влияет» — разные вещи. */
    Engine& eng = engine();
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().todos.clear();
        eng.state().todos.push_back(todo("1", "pending"));
        eng.state().todos.push_back(todo("2", "pending"));
        eng.state().prompt_dirty = false;
    }

    ASSERT_TRUE(eng.toggle_todo("1"));
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        ASSERT_EQ(eng.state().todos[0].status, std::string("completed"));
        /* Соседний пункт не тронут. */
        ASSERT_EQ(eng.state().todos[1].status, std::string("pending"));
        /* Кэш сброшен: иначе модель видела бы старый план. */
        ASSERT_TRUE(eng.state().prompt_dirty.load());
    }

    /* Уборка за собой: состояние — синглтон (правило 5). */
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().todos.clear();
    }
}

TEST(клик_по_несуществующему_пункту_не_сбрасывает_кэш) {
    /* План успел измениться между кадром и кликом: сбрасывать кэш не
     * из-за чего, а делать это значило бы пересобирать промпт впустую на
     * каждом промахе. */
    Engine& eng = engine();
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().todos.clear();
        eng.state().prompt_dirty = false;
    }

    ASSERT_FALSE(eng.toggle_todo("99"));
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        ASSERT_FALSE(eng.state().prompt_dirty.load());
    }
}

/* --- Граница с окном: окно рисует и зовёт движок, но не решает --- */

TEST(окно_не_правит_план_напрямую) {
    /* Правка из окна мимо `Engine::toggle_todo` не сбросила бы кэш
     * промпта, и человек увидел бы галочку при плане, о котором модель не
     * знает: панель «работает», а на агента не влияет. */
    std::ifstream f(fs::path(__FILE__).parent_path().parent_path() /
                        "ui/coder_window.cpp",
                    std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string ui = ss.str();
    ASSERT_FALSE(ui.empty());

    const std::size_t at = ui.find("static void render_todo_panel()");
    ASSERT_TRUE(at != std::string::npos);
    const std::size_t end = ui.find("\nstatic ", at + 10);
    const std::string body =
        ui.substr(at, end == std::string::npos ? std::string::npos : end - at);

    /* Решение пришло из core. */
    ASSERT_TRUE(body.find("todo_panel::build(") != std::string::npos);
    /* Правка уходит в движок, а не пишется в состояние из окна. */
    ASSERT_TRUE(body.find("engine().toggle_todo(") != std::string::npos);
    ASSERT_TRUE(body.find("st.todos =") == std::string::npos);
    ASSERT_TRUE(body.find("apply_toggle(st.todos") == std::string::npos);
}
