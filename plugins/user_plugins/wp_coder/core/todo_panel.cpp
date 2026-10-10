// todo_panel.cpp — решение панели плана задачи (И11.7).
//
// Здесь всё, что панель показывает и всё, что она решает. Окно ImGui
// рисует готовые поля (шапка todo_panel.h).

#include "todo_panel.h"

namespace coder {
namespace todo_panel {

std::string next_status(const std::string& current) {
    /* Одно правило на оба направления, а не «всегда completed»:
     * `in_progress` иначе было бы нечем вернуть в `pending`, и человек,
     * ошибшийся с галочкой, остался бы с пунктом «выполнено», который
     * нельзя вернуть в работу. */
    if (current == "completed") return "pending";
    return "completed";
}

bool is_locked(const std::string& id) {
    /* Возврат правки идёт по идентификатору (см. apply_toggle), и пункт
     * без него вернуть нечем: нажатая галочка, которая ничего не
     * делает, хуже явного «недоступно для правки». */
    return id.empty();
}

std::string counter_line(std::size_t remaining, std::size_t total) {
    /* Незавершённых считается ОТДЕЛЬНО от общего числа: «осталось 0 из 7»
     * читается как «всё кончено» и врёт, когда семь пунктов, семь из них
     * закрыты, а один — отменён агентом. Отменённый не входит в
     * «незавершённые»: его никто уже не сделает. */
    return "Незавершённых: " + std::to_string(remaining) + " из " +
           std::to_string(total);
}

std::string empty_line() {
    return "Плана нет: агент его ещё не составил.";
}

Panel build(const std::vector<TodoItem>& todos) {
    Panel p;
    p.visible = !todos.empty();
    p.total = todos.size();

    for (const TodoItem& t : todos) {
        Item item;
        item.id = t.id;
        item.content = t.content;
        item.status = t.status;
        item.priority = t.priority;
        /* Галочка стоит по РАВЕНСТВУ строке "completed", а не по
         * «не пусто и не отменён»: иначе `cancelled` выглядел бы как
         * выполненный, а это прямо противоположный смысл. */
        item.checked = (t.status == "completed");
        item.locked = is_locked(t.id);
        /* Незавершённым считается всё, что модель не закрыла сама.
         * `cancelled` в счёт НЕ идёт (см. counter_line): отменённый
         * пункт никто уже не выполнит, и «осталось 2 из 3», где третий
         * отменён, вводит человека в заблуждение ровно на том числе,
         * ради которого он сюда смотрит. */
        if (t.status != "completed" && t.status != "cancelled") {
            ++p.remaining;
        }
        p.items.push_back(std::move(item));
    }

    p.counter_line = counter_line(p.remaining, p.total);
    p.empty_line = empty_line();
    return p;
}

bool apply_toggle(std::vector<TodoItem>& todos, const std::string& id) {
    if (id.empty()) return false;
    for (TodoItem& t : todos) {
        if (t.id != id) continue;
        t.status = next_status(t.status);
        return true;
    }
    /* Идентификатор не найден: план изменился между кадром и кликом
     * (агент переписал список), и правка по старому идентификатору
     * изменила бы не тот пункт. Лучше не применить ничего, чем не тот
     * пункт. */
    return false;
}

}  // namespace todo_panel
}  // namespace coder