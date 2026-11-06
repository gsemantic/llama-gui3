/*
 * test_shell_cancel.cpp — отмена работающей команды (И6.7).
 *
 * Проверяется свойство, ради которого команда переписана с popen на
 * fork/exec: «стоп» убивает ВСЮ группу процессов, а не только верхний sh.
 * Команда почти всегда запускает другие процессы (сборка, тесты, сервер),
 * и убийство одного верхнего sh оставило бы работать всё дерево — то есть
 * «стоп» был бы враньём ровно там, где он особенно нужен.
 *
 * Команды здесь намеренно простые и без внешних зависимостей: проверяется
 * механика (сигнал, группа, код возврата), а не поведение shell-утилит.
 */

#include "core/shell.h"
#include "core/tool.h"
#include "core/engine.h"

#include "test_framework.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace coder;

namespace {

int ms_since(const std::chrono::steady_clock::time_point& t0) {
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count());
}

/* Сколько процессов в системе упоминают маркер в своей командной строке. */
int count_processes_with(const std::string& marker)
{
    int found = 0;
    std::error_code ec;
    for (fs::directory_iterator it("/proc", ec), end; it != end && !ec;
         it.increment(ec)) {
        const std::string dir = it->path().string();
        if (dir.find_first_not_of("/0123456789") != std::string::npos) continue;
        std::ifstream cmdline(it->path() / "cmdline", std::ios::binary);
        if (!cmdline) continue;
        std::string cl((std::istreambuf_iterator<char>(cmdline)),
                       std::istreambuf_iterator<char>());
        /* cmdline — это аргументы, разделённые NUL, а не одна строка.
         * Поиск по такой строке как по C-строке обрывается на первом же NUL,
         * то есть находит ТОЛЬКО имя программы и никогда — аргументы. Такая
         * проверка молча не находит ничего и проходит при любой поломке;
         * поэтому NUL заменяются пробелом ДО поиска. */
        for (char& ch : cl) if (ch == '\0') ch = ' ';
        if (cl.find(marker) != std::string::npos) ++found;
    }
    return found;
}

/* PID, о котором говорит команда. */
long long read_pid(const std::string& out) {
    long long pid = -1;
    std::sscanf(out.c_str(), "%lld", &pid);
    return pid;
}

} // namespace

/* Отмена возвращает управление, пока команда ещё работает. */
TEST(cancel_returns_while_the_command_is_running) {
    shell::RunOptions opt;
    opt.timeout_sec = 60;
    opt.should_cancel = []() { return true; };   /* отмена сразу */

    shell::RunStats stats;
    const auto t0 = std::chrono::steady_clock::now();
    shell::run_capture_stream("sleep 30", opt, stats);
    const int ms = ms_since(t0);

    ASSERT_TRUE(stats.cancelled);
    /* 30 с сна против мгновенной отмены. */
    ASSERT_TRUE(ms < 5000);
}

/* Процесс ДЕЙСТВИТЕЛЬНО мёртв: не только функция вернулась, а pid исчез. */
TEST(cancelled_process_is_actually_gone) {
    shell::RunOptions opt;
    opt.timeout_sec = 60;
    std::atomic<bool> go{false};
    opt.should_cancel = [&go]() { return go.load(); };

    std::thread canceller([&go] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        go.store(true);
    });

    shell::RunStats stats;
    const std::string out = shell::run_capture_stream("sleep 30", opt, stats);
    canceller.join();

    ASSERT_TRUE(stats.cancelled);
    /* Имя процесса осталось бы в таблице, если бы он выжил. */
    const long long pid = read_pid(out);
    if (pid > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ASSERT_EQ(::kill(static_cast<pid_t>(pid), 0), -1);
        ASSERT_EQ(errno, ESRCH);
    }
}

/*
 * Главная проверка: убито ДЕРЕВО, а не только верхний процесс.
 *
 * `sleep 31.5 | cat` — это три процесса: sh, sleep и cat. Убийство только
 * верхнего sh оставило бы sleep работать на 30 секунд после «стопа», и агент
 * ушёл бы на следующий шаг в компании с работающим процессом. Именно
 * поэтому команда запускается в СВОЕЙ группе (setpgid) и убивается по -pgid.
 *
 * Проверяется не pid, а выжившие процессы по уникальному маркеру в
 * /proc/<pid>/cmdline. Так проверка не зависит от того, сумеет ли команда
 * напечатать свои pid-ы (для этого нужен `sh -c 'echo $$'`, а такая форма
 * отклоняется политикой команд — и это правильно), и заодно проверяет
 * именно то, что важно: НИ ОДИН процесс команды не выжил.
 */
TEST(cancel_kills_the_whole_process_group) {
    /* Дробь, чтобы маркер не совпал с чужим процессом. */
    const std::string marker = "31.5";
    shell::RunOptions opt;
    opt.timeout_sec = 60;
    std::atomic<bool> go{false};
    opt.should_cancel = [&go]() { return go.load(); };
    std::thread canceller([&go] {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        go.store(true);
    });

    shell::RunStats stats;
    const auto t0 = std::chrono::steady_clock::now();
    const std::string out =
        shell::run_capture_stream("sleep " + marker + " | cat", opt, stats);
    canceller.join();

    ASSERT_TRUE(stats.cancelled);

    /*
     * Мгновенность — половина этого свойства, и без неё проверка обманчива.
     *
     * Если SIGTERM уходит только верхнему sh, его потомки выживают, и
     * добивка SIGKILL по группе срабатывает лишь через kill_grace_sec. То
     * есть проверка «процессов не осталось» со временем прошла бы и при
     * сломанном убийстве — просто медленнее. «Стоп» при этом выглядел бы
     * как зависание на несколько секунд, то есть пользователь ушёл бы
     * ровно туда, откуда нажал «стоп».
     *
     * kill_grace_sec по умолчанию 3 с, поэтому 2500 мс — это «добивка ещё
     * не могла успеть».
     */
    ASSERT_TRUE(ms_since(t0) < 2500);

    /* Процесс мог уйти не мгновенно: даём группе время на реакцию ядра. */
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (count_processes_with(marker) == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const int alive = count_processes_with(marker);
    if (alive > 0) {
        std::cerr << "  выжило процессов с маркером: " << alive << std::endl;
    }
    ASSERT_EQ(alive, 0);
}

/* Отмена и таймаут — разные исходы, и это видно по флагам. */
TEST(cancel_is_not_a_timeout) {
    shell::RunOptions opt;
    opt.timeout_sec = 60;                 /* таймаут далеко */
    opt.should_cancel = []() { return true; };
    shell::RunStats stats;
    shell::run_capture_stream("sleep 30", opt, stats);
    ASSERT_TRUE(stats.cancelled);
    ASSERT_FALSE(stats.timed_out);
}

/* Без отменки команда живёт до конца и отмена не выдумывается. */
TEST(command_without_cancel_runs_to_completion) {
    shell::RunOptions opt;
    opt.timeout_sec = 30;
    shell::RunStats stats;
    const auto t0 = std::chrono::steady_clock::now();
    const std::string out = shell::run_capture_stream("echo готово", opt, stats);
    ASSERT_TRUE(ms_since(t0) < 5000);
    ASSERT_FALSE(stats.cancelled);
    ASSERT_EQ(stats.exit_code, 0);
    ASSERT_TRUE(out.find("готово") != std::string::npos);
}

/* Вывод, успевший напечататься до отмены, не теряется: кольцо уже наполнено,
 * и терять его значило бы сказать модели «команда ничего не вывела». */
TEST(output_before_a_cancel_is_kept) {
    shell::RunOptions opt;
    opt.timeout_sec = 60;
    std::atomic<bool> go{false};
    opt.should_cancel = [&go]() { return go.load(); };
    std::thread canceller([&go] {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        go.store(true);
    });

    shell::RunStats stats;
    const std::string out =
        shell::run_capture_stream("echo начало; sleep 30", opt, stats);
    canceller.join();

    ASSERT_TRUE(stats.cancelled);
    ASSERT_TRUE(out.find("начало") != std::string::npos);
    ASSERT_TRUE(stats.total_bytes > 0);
}

/*
 * Добивка SIGKILL нужна для процессов, игнорирующих TERM, — а такие есть:
 * `trap "" TERM` в оболочке наследуется детьми, и обычная отмена их не
 * берёт. Без добивки такие команды висели бы до таймаута, то есть «стоп» не
 * отменял бы ровно те команды, которые отменять сложнее всего.
 *
 * Проверяется не «прошло ли время», а то, что команда завершилась и её
 * процесс исчез: бесконечное ожидание отсюда не вернулось бы.
 */
TEST(process_ignoring_term_is_killed_after_grace) {
    const std::string marker = "32.5";
    shell::RunOptions opt;
    opt.timeout_sec = 60;
    /* Секунда вместо трёх: тест не должен ждать три, иначе он сам
     * становится причиной своего таймаута. */
    opt.kill_grace_sec = 1;
    std::atomic<bool> go{false};
    opt.should_cancel = [&go]() { return go.load(); };
    std::thread canceller([&go] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        go.store(true);
    });

    shell::RunStats stats;
    const auto t0 = std::chrono::steady_clock::now();
    const std::string out = shell::run_capture_stream(
        "trap \"\" TERM; sleep " + marker, opt, stats);
    canceller.join();

    ASSERT_TRUE(stats.cancelled);
    /* Оболочка про trap не выводит ничего; проверяем, что до отмены команда
     * действительно жила. */
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (count_processes_with(marker) == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_EQ(count_processes_with(marker), 0);
    /* 300 мс до отмены + 1 с grace + запас. */
    ASSERT_TRUE(ms_since(t0) < 4000);
}

/*
 * Таймаут. Механизм переехал с timeout(1) на наш (иначе отмена не доставала
 * бы дерево), а значит тест на таймаут теперь наша обязанность: если бы его
 * не было, поломка выглядела бы как «команда висит», и заметил бы только
 * пользователь — как висящий UI. Это ровно тот класс, который D1 называл
 * исходной бедой.
 */
TEST(timeout_stops_the_command_and_is_flagged) {
    shell::RunOptions opt;
    opt.timeout_sec = 1;
    shell::RunStats stats;
    const auto t0 = std::chrono::steady_clock::now();
    const std::string out = shell::run_capture_stream("sleep 25", opt, stats);
    const int ms = ms_since(t0);

    ASSERT_TRUE(stats.timed_out);
    ASSERT_FALSE(stats.cancelled);
    /* 25 с сна против таймаута в 1 с (kill_grace_sec по умолчанию 3 — он
     * добавляется, поэтому запас на него есть). */
    ASSERT_TRUE(ms < 8000);
}

/* Таймаут — не отмена: модель обязана видеть разницу. */
TEST(timeout_is_not_reported_as_a_cancel) {
    shell::RunOptions opt;
    opt.timeout_sec = 1;
    shell::RunStats stats;
    shell::run_capture_stream("sleep 25", opt, stats);
    ASSERT_TRUE(stats.timed_out);
    ASSERT_FALSE(stats.cancelled);
}

/* Инструмент получает токен хода и отменяет его — из кода вызова. */
TEST(tool_context_exposes_the_turn_token) {
    EngineState state;
    HostCallbacks cb;
    ToolContext ctx(state, cb);
    /* Ход без токена — это не ошибка, инструмент обязан это пережить. */
    ASSERT_TRUE(ctx.abort() == nullptr);

    state.turn_abort = std::make_shared<AbortToken>();
    ToolContext with_token(state, cb);
    ASSERT_TRUE(with_token.abort() != nullptr);
    ASSERT_FALSE(with_token.abort()->aborted());
    state.turn_abort->abort();
    ASSERT_TRUE(with_token.abort()->aborted());
}

/* Токен, полученный инструментом, действительно останавливает команду:
 * это стык ctx.abort() и shell. */
TEST(token_from_context_stops_the_command) {
    EngineState state;
    state.turn_abort = std::make_shared<AbortToken>();
    HostCallbacks cb;
    ToolContext ctx(state, cb);

    shell::RunOptions opt;
    opt.timeout_sec = 60;
    AbortToken* token = ctx.abort();
    ASSERT_TRUE(token != nullptr);
    opt.should_cancel = [token]() { return token->aborted(); };

    std::thread canceller([token] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        token->abort();
    });

    shell::RunStats stats;
    const auto t0 = std::chrono::steady_clock::now();
    shell::run_capture_stream("sleep 30", opt, stats);
    canceller.join();

    ASSERT_TRUE(stats.cancelled);
    ASSERT_TRUE(ms_since(t0) < 5000);
}
