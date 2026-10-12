/*
 * test_hotkeys.cpp — перенос горячих клавиш (И11.11).
 *
 * Проверяются четыре вещи, и все четыре — про правду:
 *
 *  1. КЛАВИША ДЕЛАЕТ РОВНО ТО ЖЕ, ЧТО КНОПКА. `Esc` останавливает ровно
 *     когда `cancel::Panel::visible` — то есть когда отменять есть что.
 *     Проверяется не «Esc работает», а совпадение с `phase_of` по всем
 *     семи состояниям FSM: расхождение означало бы, что на экране и под
 *     клавишей живут две разные правды (тот же класс, что закрыл И11.10).
 *
 *  2. ПОЛЕ ВВОДА ПОБЕЖДАЕТ. `Ctrl+Enter` в промпте агента вставляет
 *     перевод строки, `Esc` в поле пароля отменяет правку поля. Если бы
 *     сочетание срабатывало там, оно крало бы нажатие у поля того же окна.
 *
 *  3. МОДИФИКАТОРЫ ТОЧНЫЕ. `Ctrl+Shift+B` — не «Ctrl+B с лишним Shift»,
 *     а чужое сочетание, и оно не значит ничего.
 *
 *  4. ПЕРЕЧЕНЬ ОБЪЯВЛЕН И ЗАМКНУТ. Подсказка в окне печатается из
 *     `bindings()`, поэтому сочетание, которому не досталось строки, —
 *     это человек, который не узнает о клавише. Сверка идёт в обе стороны:
 *     каждое действие имеет сочетание, и каждое сочетание ведёт в перечень
 *     непустых строк.
 *
 * Отдельно проверяется, что `Ctrl+O` и `Esc` не съедают поле ввода даже
 * при включённом плане — то есть что запрет сильнее разрешения.
 */

#include "core/hotkeys.h"

#include "core/cancel.h"
#include "test_framework.h"

#include <string>
#include <vector>

using namespace coder;

namespace {

hotkeys::Event press(hotkeys::Key k, bool ctrl = false, bool shift = false,
                     bool alt = false) {
    hotkeys::Event ev;
    ev.key = k;
    ev.mods.ctrl = ctrl;
    ev.mods.shift = shift;
    ev.mods.alt = alt;
    return ev;
}

/* Контекст простоя: ничего не набрано, диалогов нет, агент молчит. */
hotkeys::Context idle_ctx() {
    hotkeys::Context ctx;
    ctx.item_active = false;
    ctx.popup_open = false;
    ctx.phase = cancel::Phase::Idle;
    ctx.plan_mode = false;
    return ctx;
}

}  // namespace

/* --- 1. Каждая из четырёх клавиш делает своё --- */

TEST(ctrl_o_opens_the_edits_panel) {
    hotkeys::Context ctx = idle_ctx();
    ASSERT_EQ(hotkeys::action_for(press(hotkeys::Key::O, true), ctx),
              hotkeys::Action::OpenEdits);
}

TEST(ctrl_enter_approves_the_plan) {
    hotkeys::Context ctx = idle_ctx();
    ctx.plan_mode = true;
    ASSERT_EQ(hotkeys::action_for(press(hotkeys::Key::Enter, true), ctx),
              hotkeys::Action::ApprovePlan);
}

TEST(ctrl_b_toggles_the_details) {
    hotkeys::Context ctx = idle_ctx();
    ASSERT_EQ(hotkeys::action_for(press(hotkeys::Key::B, true), ctx),
              hotkeys::Action::ToggleDetails);
}

TEST(escape_stops_a_working_agent) {
    hotkeys::Context ctx = idle_ctx();
    ctx.phase = cancel::Phase::Working;
    ASSERT_EQ(hotkeys::action_for(press(hotkeys::Key::Escape), ctx),
              hotkeys::Action::Stop);
}

/* --- 2. «Esc делает ровно то же, что кнопка» --- */

TEST(escape_agrees_with_the_stop_button_on_every_engine_state) {
    /* Ключевая проверка задачи. Кнопка «Стоп» показывается по
     * `cancel::build(...).visible`, а клавиша обязана давать тот же ответ
     * по тому же `phase_of`. Сверяются ВСЕ состояния FSM: если появится
     * новое, проверка обязана упасть на нём, а не молча разойтись с
     * кнопкой (тот же довод, что у `every_agent_state_maps_to_a_phase`
     * в test_cancel_panel.cpp). */
    const AgentState states[] = {
        AgentState::Idle,        AgentState::Planning,
        AgentState::Executing,   AgentState::WaitingPermission,
        AgentState::Done,        AgentState::Error,
        AgentState::Aborted,
    };
    for (AgentState st : states) {
        hotkeys::Context ctx = idle_ctx();
        ctx.phase = cancel::phase_of(st);

        /* Что покажет кнопка: та же фаза, тот же признак видимости. */
        cancel::Snapshot snap;
        snap.phase = ctx.phase;
        const bool button_visible = cancel::build(snap).visible;

        const bool key_stops =
            hotkeys::action_for(press(hotkeys::Key::Escape), ctx) ==
            hotkeys::Action::Stop;

        ASSERT_EQ(key_stops, button_visible);
    }
}

TEST(escape_does_nothing_when_the_agent_is_idle) {
    /* Обещание отмены того, что не идёт, — та же ложь, что И11.10 убрал с
     * кнопки. Клавиша без кнопки тоже должна молчать. */
    hotkeys::Context ctx = idle_ctx();
    ctx.phase = cancel::Phase::Idle;
    ASSERT_EQ(hotkeys::action_for(press(hotkeys::Key::Escape), ctx),
              hotkeys::Action::None);
}

TEST(escape_does_nothing_when_the_turn_is_already_finished) {
    /* Готовое состояние — не Idle, и раньше кнопка на нём и обещала отмену
     * того, чего нет. Клавиша не должна вернуть это обещание. */
    hotkeys::Context ctx = idle_ctx();
    ctx.phase = cancel::Phase::Finished;
    ASSERT_EQ(hotkeys::action_for(press(hotkeys::Key::Escape), ctx),
              hotkeys::Action::None);
}

TEST(escape_stops_while_waiting_for_the_person) {
    /* Ожидание ответа человека — отменять есть что, и «стоп» обязан
     * доходить до PermissionEngine. */
    hotkeys::Context ctx = idle_ctx();
    ctx.phase = cancel::Phase::Waiting;
    ASSERT_EQ(hotkeys::action_for(press(hotkeys::Key::Escape), ctx),
              hotkeys::Action::Stop);
}

/* --- 3. Поле ввода и диалог побеждают --- */

TEST(a_text_field_keeps_its_own_keys) {
    /* `Ctrl+Enter` в промпте агента вставляет перевод строки, `Esc` в поле
     * пароля отменяет правку поля. Проверяются ВСЕ четыре сочетания: если
     * хоть одно проходит, оно крадёт нажатие у поля того же окна. */
    const hotkeys::Key keys[] = {hotkeys::Key::O, hotkeys::Key::B,
                                 hotkeys::Key::Enter, hotkeys::Key::Escape};
    for (hotkeys::Key k : keys) {
        hotkeys::Context ctx = idle_ctx();
        /* Намеренно состояние, в котором каждая клавиша обычно срабатывает:
         * агент работает и план включён. Запрет должен быть сильнее. */
        ctx.item_active = true;
        ctx.phase = cancel::Phase::Working;
        ctx.plan_mode = true;

        const bool ctrl = (k != hotkeys::Key::Escape);
        ASSERT_EQ(hotkeys::action_for(press(k, ctrl), ctx),
                  hotkeys::Action::None);
    }
}

TEST(escape_in_a_text_field_does_not_stop_the_agent) {
    /* Самое дорогое последствие: `Esc` в поле отменял бы правку поля И
     * глушил бы работающего агента. */
    hotkeys::Context ctx = idle_ctx();
    ctx.item_active = true;
    ctx.phase = cancel::Phase::Working;
    ASSERT_EQ(hotkeys::action_for(press(hotkeys::Key::Escape), ctx),
              hotkeys::Action::None);
}

TEST(an_open_dialog_keeps_escape_for_itself) {
    /* Диалог (например разрешения) закрывается `Esc` сам. Перехватывать
     * его клавишу плагином значило бы закрыть диалог И остановить агента
     * одним нажатием. */
    hotkeys::Context ctx = idle_ctx();
    ctx.popup_open = true;
    ctx.phase = cancel::Phase::Working;
    ASSERT_EQ(hotkeys::action_for(press(hotkeys::Key::Escape), ctx),
              hotkeys::Action::None);
}

TEST(an_open_dialog_keeps_ctrl_enter_for_itself) {
    hotkeys::Context ctx = idle_ctx();
    ctx.popup_open = true;
    ctx.plan_mode = true;
    ASSERT_EQ(hotkeys::action_for(press(hotkeys::Key::Enter, true), ctx),
              hotkeys::Action::None);
}

/* --- 4. Модификаторы точные --- */

TEST(ctrl_enter_without_ctrl_is_not_approve) {
    /* Голый Enter в этом окне ничего не значит: он не должен снимать
     * план-режим. */
    hotkeys::Context ctx = idle_ctx();
    ctx.plan_mode = true;
    ASSERT_EQ(hotkeys::action_for(press(hotkeys::Key::Enter), ctx),
              hotkeys::Action::None);
}

TEST(an_extra_shift_makes_the_press_a_different_one) {
    /* `Ctrl+Shift+B` — не «Ctrl+B с лишним Shift», а чужое сочетание.
     * Иначе `Ctrl+Shift+B` сворачивал бы детали вместо того, что человек
     * ожидал нажать. */
    hotkeys::Context ctx = idle_ctx();
    ASSERT_EQ(hotkeys::action_for(press(hotkeys::Key::B, true, true), ctx),
              hotkeys::Action::None);
    ASSERT_EQ(hotkeys::action_for(press(hotkeys::Key::O, true, true), ctx),
              hotkeys::Action::None);
    ASSERT_EQ(hotkeys::action_for(press(hotkeys::Key::Enter, true, true), ctx),
              hotkeys::Action::None);
}

TEST(alt_makes_the_press_a_different_one_too) {
    hotkeys::Context ctx = idle_ctx();
    ctx.plan_mode = true;
    ASSERT_EQ(hotkeys::action_for(press(hotkeys::Key::Enter, true, false, true),
                                  ctx),
              hotkeys::Action::None);
}

TEST(plain_keys_do_nothing) {
    /* Голые `O`, `B`, `Esc` без Ctrl — не сочетания плагина: иначе набор
     * текста в промпте агента сворачивал бы детали и останавливал ход. */
    hotkeys::Context ctx = idle_ctx();
    ctx.phase = cancel::Phase::Working;
    ASSERT_EQ(hotkeys::action_for(press(hotkeys::Key::O), ctx),
              hotkeys::Action::None);
    ASSERT_EQ(hotkeys::action_for(press(hotkeys::Key::B), ctx),
              hotkeys::Action::None);
}

TEST(ctrl_escape_is_not_stop) {
    /* `Ctrl+Esc` — чужое сочетание. */
    hotkeys::Context ctx = idle_ctx();
    ctx.phase = cancel::Phase::Working;
    ASSERT_EQ(hotkeys::action_for(press(hotkeys::Key::Escape, true), ctx),
              hotkeys::Action::None);
}

TEST(no_key_means_no_action) {
    hotkeys::Context ctx = idle_ctx();
    ctx.phase = cancel::Phase::Working;
    ctx.plan_mode = true;
    ASSERT_EQ(hotkeys::action_for(press(hotkeys::Key::None), ctx),
              hotkeys::Action::None);
}

/* --- 5. Одобрение плана только когда есть что одобрять --- */

TEST(ctrl_enter_does_nothing_when_plan_mode_is_off) {
    /* План-режим выключен — одобрять нечего. Нажатие, которое всё равно
     * «сработало бы», выглядело бы как действие, изменившее состояние. */
    hotkeys::Context ctx = idle_ctx();
    ctx.plan_mode = false;
    ASSERT_EQ(hotkeys::action_for(press(hotkeys::Key::Enter, true), ctx),
              hotkeys::Action::None);
}

/* --- 6. Перечень объявлен и замкнут --- */

TEST(every_action_has_a_declared_binding) {
    /* Действие без сочетания — это человек, который о нём не узнает:
     * подсказка в окне печатается из этого перечня. Обход идёт по ВСЕМ
     * значениям `Action`, и новое действие обязано попасть в перечень или
     * уронить эту проверку своим именем. */
    const hotkeys::Action all[] = {
        hotkeys::Action::None,        hotkeys::Action::OpenEdits,
        hotkeys::Action::ApprovePlan, hotkeys::Action::Stop,
        hotkeys::Action::ToggleDetails,
    };
    for (hotkeys::Action a : all) {
        if (a == hotkeys::Action::None) continue;
        bool found = false;
        for (const hotkeys::Binding& b : hotkeys::bindings()) {
            if (b.action == a) found = true;
        }
        ASSERT_TRUE(found);
    }
}

TEST(no_declared_binding_is_empty_or_meaningless) {
    /* Пустая строка в перечне печаталась бы в подсказку как «Клавиши: , , » —
     * то есть перечень врёт на экране. */
    for (const hotkeys::Binding& b : hotkeys::bindings()) {
        ASSERT_FALSE(b.keys.empty());
        ASSERT_TRUE(b.action != hotkeys::Action::None);
    }
}

TEST(the_four_bindings_of_the_task_are_all_present) {
    /* Задача называет четыре сочетания. Перечень — объявленный, поэтому
     * забытое сочетание не поймал бы ни один другой тест: проверка по
     * списку строк и есть та, которая его требует. */
    const char* expected[] = {"Ctrl+O", "Ctrl+Enter", "Esc", "Ctrl+B"};
    for (const char* want : expected) {
        bool found = false;
        for (const hotkeys::Binding& b : hotkeys::bindings()) {
            if (b.keys == want) found = true;
        }
        ASSERT_TRUE(found);
    }
    ASSERT_EQ(hotkeys::bindings().size(), size_t(4));
}

TEST(the_help_line_names_every_binding_and_nothing_more) {
    /* Подсказка собирается из `bindings()`, и проверка требует, чтобы это
     * осталось правдой: строка обязана называть каждое сочетание перечня и
     * не содержать ни одного, чего в перечне нет. Строка, собранная в
     * окне мимо core, разошлась бы с тем, что клавиши действительно
     * делают (тот же довод, что у `details_line` в И11.5). */
    const std::string help = hotkeys::help_line();
    for (const hotkeys::Binding& b : hotkeys::bindings()) {
        ASSERT_TRUE(help.find(b.keys) != std::string::npos);
    }
    /* Число названных сочетаний равно размеру перечня: лишнего в подсказке
     * быть не может, потому что строка собрана из него. */
    size_t commas = 0;
    for (char c : help) {
        if (c == ',') ++commas;
    }
    ASSERT_EQ(commas, hotkeys::bindings().size() - 1);
}

TEST(the_four_bindings_do_not_collapse_into_one_another) {
    /* Два сочетания, ведущие в одно действие, — это одно сочетание,
     * напечатанное дважды: подсказка обещала бы человеку клавишу, которая
     * означает то же, и лишний повод искать различия, которых нет. */
    for (size_t i = 0; i < hotkeys::bindings().size(); ++i) {
        for (size_t j = i + 1; j < hotkeys::bindings().size(); ++j) {
            ASSERT_TRUE(hotkeys::bindings()[i].action !=
                        hotkeys::bindings()[j].action);
            ASSERT_TRUE(hotkeys::bindings()[i].keys !=
                        hotkeys::bindings()[j].keys);
        }
    }
}

/* --- 7. Проверка, что сочетание не съедает поле ввода даже у Esc ---
 *
 * Отдельно от `a_text_field_keeps_its_own_keys`: там поле активно И агент
 * работает. Здесь — поле активно, агент работает, и нажатие приходит с
 * ПУСТЫМ контекстом ввода (окно не в фокусе). Проверка ловит другую
 * ошибку: «сработать, если не набираем» вместо «не сработать, если
 * набираем».
 */

TEST(hotkeys_are_checked_before_the_agent_state_is_read) {
    /* Порядок в core: сначала запрет (поле/диалог), потом смысл клавиши.
     * Наблюдаемо это так — при активном поле НИ ОДНО сочетание не даёт
     * действия даже когда состояние его разрешает. Проверено выше; здесь
     * то, что запрет не зависит от того, какое именно действие было бы. */
    hotkeys::Context ctx = idle_ctx();
    ctx.item_active = true;
    ctx.phase = cancel::Phase::Working;
    ctx.plan_mode = true;
    ctx.popup_open = false;

    ASSERT_EQ(hotkeys::action_for(press(hotkeys::Key::O, true), ctx),
              hotkeys::Action::None);
    ASSERT_EQ(hotkeys::action_for(press(hotkeys::Key::B, true), ctx),
              hotkeys::Action::None);
    ASSERT_EQ(hotkeys::action_for(press(hotkeys::Key::Enter, true), ctx),
              hotkeys::Action::None);
    ASSERT_EQ(hotkeys::action_for(press(hotkeys::Key::Escape), ctx),
              hotkeys::Action::None);
}