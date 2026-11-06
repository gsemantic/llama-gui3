#include "hotkeys.h"

#include <ostream>

namespace coder {
namespace hotkeys {

namespace {

/* Нажатие без модификаторов и с лишним модификатором — РАЗНЫЕ нажатия.
 * `Ctrl+Shift+B` — это не «Ctrl+B с лишним Shift», это другое сочетание, и
 * оно ничего не значит. Сравнение поэтому точное, а не «модификатор
 * на месте». */
bool mods_are(const Mods& m, bool ctrl) {
    return m.ctrl == ctrl && !m.shift && !m.alt;
}

}  // namespace

std::string action_name(Action action) {
    switch (action) {
        case Action::OpenEdits:     return "OpenEdits";
        case Action::ApprovePlan:   return "ApprovePlan";
        case Action::Stop:          return "Stop";
        case Action::ToggleDetails: return "ToggleDetails";
        case Action::None:          break;
    }
    return "None";
}

std::ostream& operator<<(std::ostream& os, Action action) {
    return os << action_name(action);
}

Action action_for(const Event& ev, const Context& ctx) {
    /* Поле ввода и диалог побеждают. Не «кроме случаев», а всегда: иначе
     * `Ctrl+Enter` в промпте агента вставил бы перевод строки И снял
     * план-режим, а `Esc` в поле пароля отменял бы правку поля И глушил
     * агента. Клавиатура плагина не должна отнимать нажатия у полей того же
     * окна (шапка hotkeys.h). */
    if (ctx.item_active || ctx.popup_open) return Action::None;

    switch (ev.key) {
        case Key::O:
            /* Открыть правки — можно всегда: это переход к панели, а не
             * действие над состоянием агента. */
            return mods_are(ev.mods, true) ? Action::OpenEdits : Action::None;

        case Key::B:
            return mods_are(ev.mods, true) ? Action::ToggleDetails : Action::None;

        case Key::Enter:
            /* Одобрить план — ровно то, что делает чекбокс план-режима, и
             * только когда режим включён: выключенный план-режим означает,
             * что одобрять нечего, и нажатие не сделало бы ничего, но
             * выглядело бы как сработавшее. */
            if (!mods_are(ev.mods, true)) return Action::None;
            return ctx.plan_mode ? Action::ApprovePlan : Action::None;

        case Key::Escape:
            /* Без модификаторов: `Ctrl+Esc` — чужое сочетание. */
            if (ev.mods.ctrl || ev.mods.shift || ev.mods.alt)
                return Action::None;
            /* Ровно то же условие, что у кнопки «Стоп»: `phase_of` даёт
             * Working/Waiting — отменять есть что. На Finished и Idle
             * нажатие было бы обещанием отмены того, что не идёт, — та же
             * ложь, что закрыл И11.10, только с клавиатуры. */
            if (ctx.phase == cancel::Phase::Working ||
                ctx.phase == cancel::Phase::Waiting) {
                return Action::Stop;
            }
            return Action::None;

        case Key::None:
            break;
    }
    return Action::None;
}

const std::vector<Binding>& bindings() {
    /* Объявленный перечень, а не вычисляемый из кода выше: он печатается в
     * подсказке, и действие, которому не досталось сочетания, обязано быть
     * здесь — иначе человек не узнает о нём, а проверка это поймает. */
    static const std::vector<Binding> kBindings = {
        {"Ctrl+O", Action::OpenEdits},
        {"Ctrl+Enter", Action::ApprovePlan},
        {"Esc", Action::Stop},
        {"Ctrl+B", Action::ToggleDetails},
    };
    return kBindings;
}

std::string help_line() {
    /* Собирается из того же перечня, что и решение, — одна строка о сочетаниях
     * не может разойтись с тем, что они делают. */
    std::string out = "Клавиши: ";
    const std::vector<Binding>& b = bindings();
    for (size_t i = 0; i < b.size(); ++i) {
        if (i) out += ", ";
        out += b[i].keys;
    }
    return out;
}

}  // namespace hotkeys
}  // namespace coder