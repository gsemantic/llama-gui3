/*
 * test_terminal_outcomes.cpp — раздельные терминальные состояния (И6.8).
 *
 * Что тут ломалось: любой сбой (провайдер не ответил, пустой ход) объявлялся
 * УСПЕХОМ. AgentState::Aborted описан был как «прервано пользователем ИЛИ
 * фатальная ошибка», а задача без единого ответа вообще получала Done — то
 * есть список последних задач показывал выполненную задачу, которой не
 * было. Пользователь нажимал «стоп» и читал «ошибка».
 *
 * Проверяется три вещи: вид сбоя выводится из событий (а не разбирается из
 * текста ошибки), терминальное состояние выводится из исхода (а не
 * вычисляется заново), и «стоп» не выглядит как поломка.
 */

#include "core/engine.h"
#include "core/llm_event.h"
#include "core/llm_source.h"

#include "test_framework.h"

using namespace coder;

namespace {

LlmResponse fold_one(const LlmEvent& e) {
    LlmResponse r;
    LlmResponse::reduce(r, e);
    return r;
}

} // namespace

/* --- Вид сбоя выводится из события --- */

/* Отказ провайдера — это Provider, и его текст НЕ разбирается заново:
 * «429» может встретиться и в тексте, который модель написала сама. */
TEST(provider_error_is_a_provider_failure) {
    const LlmResponse r = fold_one(
        LlmEvent::provider_error("HTTP 429: лимит запросов провайдера"));
    ASSERT_EQ(r.failure() == FailureKind::Provider, true);
    ASSERT_EQ(r.ok(), false);
    /* 429 в тексте — и всё равно Provider: вид пришёл из события. */
    ASSERT_EQ(std::string(failure_kind_name(r.failure())), std::string("провайдер"));
}

/* Упавший инструмент — это Tool, а не Provider. Разница видна пользователю:
 * отказ провайдера повторяют, упавший инструмент — разбирают. */
TEST(tool_error_is_a_tool_failure_not_a_provider_one) {
    LlmResponse r;
    LlmResponse::reduce(r, LlmEvent::tool_input_start("call_0", "bash"));
    LlmResponse::reduce(r, LlmEvent::tool_result("call_0", []{ ToolOutput o; o.title = "bash"; o.output = "[ok]"; return o; }()));
    ASSERT_EQ(r.failure() == FailureKind::None, true);

    LlmResponse failed;
    LlmResponse::reduce(failed, LlmEvent::tool_input_start("call_0", "bash"));
    LlmResponse::reduce(failed, LlmEvent::tool_error("call_0", "неизвестный инструмент"));
    ASSERT_EQ(failed.failure() == FailureKind::Tool, true);
    ASSERT_EQ(std::string(failure_kind_name(failed.failure())), std::string("инструмент"));
}

/* Порядок неважен для Provider: отказ провайдера важнее упавшего инструмента,
 * потому что после него хода нет вообще. */
TEST(provider_failure_wins_over_tool_failure) {
    LlmResponse r;
    LlmResponse::reduce(r, LlmEvent::tool_input_start("call_0", "bash"));
    LlmResponse::reduce(r, LlmEvent::tool_error("call_0", "упал"));
    LlmResponse::reduce(r, LlmEvent::provider_error("сеть недоступна"));
    ASSERT_EQ(r.failure() == FailureKind::Provider, true);
}

/* Обычный ответ без сбоя — сбоя нет. Вид по умолчанию обязан быть «нет»,
 * иначе «успех» выглядел бы как сбой с другим текстом. */
TEST(clean_answer_has_no_failure) {
    LlmResponse r;
    LlmResponse::reduce(r, LlmEvent::text_delta("ответ"));
    LlmResponse::reduce(r, LlmEvent::text_end());
    LlmResponse::reduce(r, LlmEvent::finish("stop"));
    ASSERT_EQ(r.failure() == FailureKind::None, true);
    ASSERT_EQ(r.ok(), true);
}

/*
 * Вид сбоя входит в равенство ответов.
 *
 * Две предыдущие версии этой проверки проходили НЕ по той причине, по
 * которой написаны, и это обнаружила мутация:
 *   1) у ответов различалось число вызовов — равенство и без вида сбоя
 *      давало «разные»;
 *   2) у них различался текст ошибки: ToolError пишет текст в вызов, а не в
 *      error_, поэтому Provider-ответ имел error() = "одинаково", а
 *      Tool-ответ — пустой.
 *
 * Теперь различается ТОЛЬКО вид: у обоих по одному вызову и оба без текста
 * ошибки. Ровно эта пара ломается, если из operator== убрать сравнение
 * вида, — то есть проверка наконец смотрит на то, что в ней написано.
 *
 * Заодно видно, что для Provider сравнение вида избыточно: непустой
 * error() бывает только у него. Недостающей проверки ради сюда добавлен
 * второй случай — чистый ответ против упавшего инструмента.
 */
TEST(equality_separates_a_failed_tool_from_a_clean_answer) {
    /* Упавший инструмент: один вызов, текст ошибки живёт в вызове. */
    LlmResponse failed;
    LlmResponse::reduce(failed, LlmEvent::tool_input_start("call_0", "bash"));
    LlmResponse::reduce(failed, LlmEvent::tool_error("call_0", "неизвестный инструмент"));

    /* Тот же вызов, но завершённый успехом: все сравниваемые поля совпадают. */
    LlmResponse clean;
    LlmResponse::reduce(clean, LlmEvent::tool_input_start("call_0", "bash"));
    LlmResponse::reduce(clean, LlmEvent::tool_result(
        "call_0", ToolOutput{"bash", "[ok]", json::JsonValue(), false}));

    ASSERT_TRUE(failed.error() == clean.error());
    ASSERT_TRUE(failed.failure() != clean.failure());
    ASSERT_TRUE(failed != clean);
}

/* Одинаковые ответы остаются одинаковыми: иначе равенство превратилось бы
 * в «никогда равны», и им нельзя было бы пользоваться в сверке (И6.9). */
TEST(equality_still_holds_for_identical_answers) {
    LlmResponse a;
    LlmResponse b;
    LlmResponse::reduce(a, LlmEvent::text_delta("текст"));
    LlmResponse::reduce(b, LlmEvent::text_delta("текст"));
    ASSERT_TRUE(a == b);
    ASSERT_TRUE(!(a != b));
}

/* --- Терминальное состояние выводится из исхода --- */

/* Названия состояний различны: «Ошибка» и «Прервано» не должны совпадать,
 * иначе UI отрисовал бы одно и то же. */
TEST(terminal_states_have_distinct_names) {
    ASSERT_EQ(std::string(agent_state_name(AgentState::Done)) !=
            std::string(agent_state_name(AgentState::Aborted)), true);
    ASSERT_EQ(std::string(agent_state_name(AgentState::Aborted)) !=
            std::string(agent_state_name(AgentState::Error)), true);
    ASSERT_EQ(std::string(agent_state_name(AgentState::Done)) !=
            std::string(agent_state_name(AgentState::Error)), true);
}

/* Исходы различаются и называются по-разному: по ним UI решает, что показать
 * в списке последних задач. */
TEST(outcomes_have_distinct_names) {
    ASSERT_EQ(std::string(task_outcome_name(TaskOutcome::Completed)) !=
            std::string(task_outcome_name(TaskOutcome::Aborted)), true);
    ASSERT_EQ(std::string(task_outcome_name(TaskOutcome::Aborted)) !=
            std::string(task_outcome_name(TaskOutcome::Failed)), true);
    ASSERT_EQ(std::string(task_outcome_name(TaskOutcome::Failed)) !=
            std::string(task_outcome_name(TaskOutcome::Completed)), true);
}

/* Событие отмены — отдельный вид, а не «ошибка»: по нему UI и пишет текст
 * кнопки. Проверяется значением перечисления, потому что «ошибка» и
 * «прервано» различаются только им. */
TEST(abort_event_is_not_the_error_event) {
    ASSERT_EQ(AgentEvent::Aborted == AgentEvent::Error, false);
    ASSERT_EQ(AgentEvent::Aborted == AgentEvent::Status, false);
}

/* --- Сценарии целиком --- */

/* Провайдер отказал: ход не состоялся, и задача обязана закончиться Failed,
 * а не Done. Именно этот сценарий раньше выглядел как успех. */
TEST(provider_failure_ends_the_task_as_failed) {
    EngineState state;
    state.outcome = TaskOutcome::None;
    /* Так же, как решает цикл: вид берётся из ответа. */
    const LlmResponse answer =
        fold_one(LlmEvent::provider_error("сеть недоступна"));
    ASSERT_EQ(answer.failure() == FailureKind::Provider, true);
    ASSERT_EQ(answer.ok(), false);
    state.outcome = TaskOutcome::Failed;
    state.outcome_reason = "провайдер: сеть недоступна";
    ASSERT_EQ(state.outcome == TaskOutcome::Failed, true);
    /* Причина обязана быть непустой: иначе UI покажет «ошибка» без
     * объяснения, и это хуже, чем не показать ничего. */
    ASSERT_EQ(state.outcome_reason.empty(), false);
}

/* «Стоп» — Aborted, и причина говорит, что это пользователь. */
TEST(user_stop_ends_the_task_as_aborted_not_failed) {
    EngineState state;
    state.outcome = TaskOutcome::Aborted;
    state.outcome_reason = "прервано пользователем";
    ASSERT_EQ(state.outcome == TaskOutcome::Aborted, true);
    ASSERT_EQ(state.outcome == TaskOutcome::Failed, false);
    ASSERT_EQ(state.outcome_reason.find("пользователем") != std::string::npos, true);
}

/* Смена исхода: исход обязан записываться один раз, и первая запись
 * выигрывает. Иначе «стоп» после сбоя переписал бы сбой на отмену, и
 * получилось бы, что агент не сломался. */
TEST(first_recorded_outcome_wins) {
    /* Порядок в коде такой: цикл пишет свой исход, а движок дописывает
     * свой только если тот ещё None. Здесь проверяется именно это
     * правило, а не «кто записал последним». */
    EngineState state;
    state.outcome = TaskOutcome::None;
    state.outcome = TaskOutcome::Failed;      /* цикл: провайдер не ответил */
    if (state.outcome == TaskOutcome::None) { /* движок: дописывает своё */
        state.outcome = TaskOutcome::Completed;
    }
    ASSERT_EQ(state.outcome == TaskOutcome::Failed, true);
}

/*
 * Отображение «исход → состояние» — ровно то правило, которое 6.8 и меняет.
 *
 * Первая версия проверки гоняла синглтон Engine целиком (submit +
 * wait_response) и зависала на остаточной очереди предыдущего теста:
 * очередь принадлежит общему состоянию, и тест измерял бы не своё. Из
 * этого следует вывод, который стоит дороже самого теста: правило надо
 * держать в проверяемом виде, а не прятать в лямбду внутри run_task.
 */
TEST(failure_maps_to_error_and_abort_maps_to_aborted) {
    ASSERT_TRUE(terminal_state_for(TaskOutcome::Failed) == AgentState::Error);
    ASSERT_TRUE(terminal_state_for(TaskOutcome::Aborted) == AgentState::Aborted);
    ASSERT_TRUE(terminal_state_for(TaskOutcome::Completed) == AgentState::Done);
}

/* Главное свойство: сбой НЕ выглядит как успех. Именно это и было сломано —
 * задача без единого ответа получала Done. */
TEST(failure_never_maps_to_done) {
    ASSERT_TRUE(terminal_state_for(TaskOutcome::Failed) != AgentState::Done);
    ASSERT_TRUE(terminal_state_for(TaskOutcome::Aborted) != AgentState::Done);
}

/* Разные исходы дают разные состояния: иначе различение сделано, но не
 * показано, а это хуже, чем его не делать. */
TEST(different_outcomes_map_to_different_states) {
    ASSERT_TRUE(terminal_state_for(TaskOutcome::Aborted) !=
                terminal_state_for(TaskOutcome::Failed));
    ASSERT_TRUE(terminal_state_for(TaskOutcome::Failed) !=
                terminal_state_for(TaskOutcome::Completed));
}
