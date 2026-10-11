// cancel.cpp — индикатор отменяемости и перечень отменяемых инструментов
// (И11.10).

#include "cancel.h"

#include "timeline.h"

#include <algorithm>

namespace coder {
namespace cancel {

/* Разложение состояния движка. Единственное место, где FSM разговаривает
 * с панелью отмены. */
Phase phase_of(AgentState state) {
    switch (state) {
        /* Ход закончен — отменять нечего, и кнопка на экране была бы
         * обещанием. */
        case AgentState::Done:
        case AgentState::Error:
        case AgentState::Aborted:
            return Phase::Finished;
        case AgentState::WaitingPermission:
            return Phase::Waiting;
        case AgentState::Planning:
        case AgentState::Executing:
            return Phase::Working;
        case AgentState::Idle:
            break;
    }
    return Phase::Idle;
}

/* Перечень инструментов, которые останавливаются сами.
 *
 * ПРАВИЛО СОСТАВА: в списке только то, что действительно спрашивает токен
 * хода в своей работе. Не «всё долгое», а «всё, что уже умеет
 * останавливаться» — потому что обещание панели проверяется по этому
 * списку, и лишнее имя здесь означало бы «стоп сработает сейчас» для
 * инструмента, который до конца отработает.
 *
 * Состав по механикам (все три — существующие, И6.7/И8.12/И11.10):
 *   - bash            — SIGTERM группе процессов, потом SIGKILL;
 *   - glob, list      — обход fileglob::find, проверка на каждой записи;
 *   - grep_search, repo_map, rag_index — обход file_utils::walk_files;
 *   - docker_build, docker_run, ssh_exec, pip_install, pytest_run,
 *     deploy          — shell::run_capture_cancel, тот же путь, что у bash;
 *   - task            — каскад И8.12: токен родителя гасит вложенный ход.
 *
 * НЕ в списке (осознанно, с причиной — см. отклонение 175 плана):
 *   - пишущие инструменты: прерывание записи оставляет файл хуже, чем
 *     недописанное намерение; отмена сработает после вызова;
 *   - wp_create_site: многошаговая операция, где оборванный шаг оставляет
 *     созданный наполовину сайт; то же основание, что у пишущих;
 *   - короткие команды (git_*, nginx_test, cron_list, php_lint): у них
 *     таймаут меньше минуты, и отдельный путь исполнения стоил бы
 *     дублирования поведения ради случая, который не ждут. */
const std::vector<std::string>& interruptible_tools() {
    static const std::vector<std::string> tools = {
        "bash", "glob", "list", "grep_search", "repo_map", "rag_index",
        "task", "docker_build", "docker_run", "ssh_exec", "pip_install",
        "pytest_run", "deploy",
    };
    return tools;
}

Interrupt interrupt_of(const std::string& tool_name) {
    const std::vector<std::string>& tools = interruptible_tools();
    const auto it = std::find(tools.begin(), tools.end(), tool_name);
    return it == tools.end() ? Interrupt::AfterCall : Interrupt::Now;
}

/* Что делает «Стоп» — по снимку.
 *
 * Порядок ветвей — по тому, что человек видит раньше всего: уже запрошенная
 * отмена важнее описания того, что сейчас выполняется, потому что в этот
 * момент кнопка уже нажата, и лишнее обещание сбило бы с толку. */
std::string stop_line(const Snapshot& snap) {
    if (snap.cancel_requested) {
        return "«Стоп» уже нажат — агент останавливается.";
    }
    if (snap.waiting_permission) {
        return "«Стоп» отклонит ожидающий вопрос и остановит ход.";
    }
    if (!snap.tool_name.empty()) {
        if (interrupt_of(snap.tool_name) == Interrupt::Now) {
            return "«Стоп» остановит " + snap.tool_name +
                   " прямо сейчас — работа уже идёт.";
        }
        return "«Стоп» дождётся конца вызова " + snap.tool_name +
               ": внутри него прерываться нельзя, следующий вызов не"
               " запустится.";
    }
    /* Инструмента нет — агент ждёт модель. */
    return "«Стоп» оборвёт ожидание ответа модели.";
}

/* Строка «что происходит». Активный инструмент — тот же, что у панели
 * стриминга (И11.6): он берётся из истории, а не из второго поля
 * состояния. */
static std::string head_line(const Snapshot& snap) {
    if (snap.cancel_requested) return "Останавливается...";
    if (snap.waiting_permission)
        return "Ждёт вашего решения"
               + (snap.state_name.empty() ? "" : " (" + snap.state_name + ")");
    if (!snap.tool_name.empty()) {
        std::string line = "Выполняется " + snap.tool_name;
        /* Время — тем же форматом, что таймлайн (И11.3): одна длительность
         * обязана печататься одинаково в окне и в проверке. */
        if (snap.tool_elapsed_ms > 0)
            line += ", " + timeline::duration_text(snap.tool_elapsed_ms);
        return line;
    }
    return "Думает"
           + (snap.state_name.empty() ? "" : " (" + snap.state_name + ")");
}

Panel build(const Snapshot& snap) {
    Panel panel;
    /* Панели нет, когда отменять нечего: пустой блок с кнопкой выглядел бы
     * как поломка индикатора, а не как «агент свободен». Законченный ход
     * (Done/Error/Aborted) — тоже нечего отменять, хотя состояние движка
     * ещё не Idle. */
    if (snap.phase == Phase::Idle || snap.phase == Phase::Finished)
        return panel;

    panel.visible = true;
    panel.stopping = snap.cancel_requested;
    panel.head_line = head_line(snap);
    panel.stop_line = stop_line(snap);
    panel.stop_now = !snap.cancel_requested && !snap.waiting_permission &&
                     (snap.tool_name.empty() ||
                      interrupt_of(snap.tool_name) == Interrupt::Now);
    return panel;
}

}  // namespace cancel
}  // namespace coder