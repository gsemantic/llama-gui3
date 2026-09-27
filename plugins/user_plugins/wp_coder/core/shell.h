#pragma once

/*
 * shell.h — безопасные обёртки вокруг системных команд.
 *
 * Все вызовы команд снабжаются таймаутом (timeout(1)), чтобы зависший
 * процесс не вешал worker-поток агента навсегда. Аргументы экранируются
 * через shell_quote() (single-quote), чтобы кавычки из текста модели не
 * ломали команду и не позволяли инъекцию.
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>

#include "json_utils.h"
#include "security.h"

namespace coder {
namespace shell {

/* И3.6: политика команд на входе shell.
 *
 * Это единственная точка, где команда, СОБРАННАЯ плагином из аргументов
 * модели, уходит в процесс: run_capture и run_capture_status зовут
 * popen, а всё исполнение в core/ и modules/ идёт через них. Проверка
 * здесь, а не в каждом из 50 инструментов, потому что проверка, которую
 * можно забыть, не является проверкой: добавление нового инструмента не
 * должно требовать второго решения «а не забыть ли политику».
 *
 * Что именно проверяется — см. security::check_assembled_command: не
 * allowlist программ (там бинарник выбрал код плагина, а не модель), а
 * все запреты по бинарнику: dd/mkfs, sudo -E, git -c core.pager=, curl
 * --upload-file, подстановки в неэкранированном аргументе.
 *
 * bash проверяет свою команду сам и строже (allowlist программ и
 * хостов), потому там бинарник выбирает модель. Повторная проверка в
 * этом случае бесплатна и безвредна: строгая проверка уже пройдена.
 *
 * Прокси-обёртка timeout(1) добавляется ПОСЛЕ проверки и в неё не
 * попадает: иначе allowlist пришлось бы пополнять словом timeout. */
inline std::string run_capture(const std::string& cmd, unsigned timeout_sec = 60) {
    std::string refusal = security::check_assembled_command(cmd);
    if (!refusal.empty()) return "[запрещено политикой команд] " + refusal;
    std::string full;
    if (timeout_sec > 0) {
        full = "timeout -k 5 " + std::to_string(timeout_sec) + " " + cmd + " 2>&1";
    } else {
        full = cmd + " 2>&1";
    }
    FILE* f = popen(full.c_str(), "r");
    if (!f) return "[ошибка] не удалось запустить: " + cmd;
    char buf[4096];
    std::string out;
    while (fgets(buf, sizeof(buf), f)) out += buf;
    pclose(f);
    return out;
}

/* Выполнить команду и вернуть true при нулевом коде возврата. */
inline bool run_capture_status(const std::string& cmd, std::string& out,
                               int& exit_code, unsigned timeout_sec = 60) {
    out.clear();
    exit_code = -1;
    std::string refusal = security::check_assembled_command(cmd);
    if (!refusal.empty()) {
        out = "[запрещено политикой команд] " + refusal;
        return false;
    }
    std::string full;
    if (timeout_sec > 0) {
        full = "timeout -k 5 " + std::to_string(timeout_sec) + " " + cmd + " 2>&1";
    } else {
        full = cmd + " 2>&1";
    }
    FILE* f = popen(full.c_str(), "r");
    if (!f) {
        out = "[ошибка] не удалось запустить: " + cmd;
        exit_code = -1;
        return false;
    }
    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) out += buf;
    exit_code = pclose(f);
    return exit_code == 0;
}

/* --- И4.8: запуск с кольцевым буфером, spill в файл и живым прогрессом
 *
 * Прежний run_capture копил весь вывод в память и отдавал первые 12 КБ.
 * Для команды вроде «npm install» или «docker build» это значило три вещи:
 * хвост вывода (где ошибка) терялся, память росла без предела, а UI до
 * конца команды не показывал ничего.
 *
 * Здесь три ответа на это:
 *   - кольцевой буфер: в вывод попадают ПОСЛЕДНИЕ ring_bytes, потому что
 *     ошибка всегда в конце, а начало уже сказано в прошлом шаге;
 *   - spill в файл: полный вывод пишется на диск по мере поступления
 *     (не «собрать в памяти, потом записать» — иначе память всё равно
 *     раздуется), и путь возвращается в вывод инструмента;
 *   - on_progress: вызывается раз в ~секунду, пока команда жива. Через
 *     него UI видит, что процесс идёт, а не завис.
 */
struct RunStats {
    int exit_code = -1;
    bool timed_out = false;
    /* И6.7: команду остановил пользователь, а не таймаут. Отдельный флаг
     * обязателен: exit_code после SIGTERM — это 143 или -1, и по нему
     * нельзя отличить «пользователь нажал Стоп» от «команда упала сама»,
     * а модель обязана видеть разницу в <shell_metadata>. */
    bool cancelled = false;
    size_t total_bytes = 0;
    /* Сколько байт выпало из кольца (того, что модель не увидит). */
    size_t dropped_bytes = 0;
    std::string spill_path;
    double elapsed_ms = 0.0;
};

struct RunOptions {
    unsigned timeout_sec = 120;
    /* Размер кольца: сколько последних байт остаётся в выводе. По умолчанию
     * ровно префилл (kMaxToolOutput): кольцо и есть то, что увидит модель,
     * поэтому держать больше бессмысленно, а обрезать кольцо потом ещё раз
     * нельзя — обрезка берёт НАЧАЛО и отрезала бы хвост, то есть ровно то
     * ради чего кольцо и затевалось. */
    size_t ring_bytes = 0;
    /* Куда писать полный вывод. Пусто — не писать (и тогда большой вывод
     * просто обрежется, о чём честно сказано в ответе). */
    std::string spill_path;
    /* Прогресс: (всего байт, последняя непустая строка). */
    std::function<void(size_t, const std::string&)> on_progress;
    /* И6.7: признак отмены. Пусто — отменять нечем, и команда живёт до
     * таймаута. */
    std::function<bool()> should_cancel;
    /* И6.7: сколько дать процессу на завершение после SIGTERM, прежде чем
     * послать SIGKILL. Ровно то, что делает timeout -k, и столько же нужно
     * здесь: без этого процесса, игнорирующего TERM (а такие есть —
     * например, джобы, ждущие ввода), пришлось бы ждать вечно. */
    unsigned kill_grace_sec = 3;
};

inline std::string run_capture_stream(const std::string& cmd,
                                      const RunOptions& opt, RunStats& stats) {
    stats = RunStats();
    std::string refusal = security::check_assembled_command(cmd);
    if (!refusal.empty()) return "[запрещено политикой команд] " + refusal;

    /*
     * Обёртка timeout(1) ЗДЕСЬ сознательно не используется (в run_capture и
     * run_capture_status — используется).
     *
     * timeout создаёт СВОЮ группу процессов, и наш kill по -pgid переставал
     * доставать дерево: команда жила до конца, несмотря на отмену. Проверено
     * тестом: с обёрткой отмена возвращалась не за 0,3 с, а за 5+.
     *
     * Таймаут здесь считает тот же цикл, что и отмена, и это единственное
     * место, где он считается: один механизм вместо двух — таймаут(1) и
     * наш SIGTERM/SIGKILL вели себя бы по-разному, и отличие выглядело бы
     * как «иногда стоп работает».
     */
    const std::string full = cmd + " 2>&1";

    /*
     * И6.7: запуск через fork/exec, а не через popen.
     *
     * popen не отдаёт pid, а без pid нельзя ни убить процесс, ни его
     * потомков. Для «стоп» это неудобство, а не мелочь: команда почти всегда
     * запускает ДРУГИЕ процессы (сборка, тесты, сервер), и убить только
     * верхний sh — значит оставить работать всё, ради чего «стоп» и жалки.
     *
     * Ребёнок делает setpgid(0, 0), то есть становится лидером своей
     * группы; убиваем мы группу по отрицательному pid, и тогда под SIGTERM
     * попадает всё дерево. setpgid дублируется в родителе: ребёнок может
     * успеть exec раньше, чем родитель выполнит свой вызов, и гонка за
     * лидерство группы — это классический источник «иногда не убивается».
     *
     * Почему не popen+timeout(1): таймаут живого процесса тут не мешает,
     * а вот доступа к pid у popen нет в принципе.
     */
    int fds[2];
    if (pipe(fds) != 0) return "[ошибка] не удалось создать канал: " + cmd;
    const pid_t pid = fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return "[ошибка] не удалось запустить: " + cmd;
    }
    if (pid == 0) {
        /* Ребёнок. Здесь нельзя трогать объекты родителя (кольцо, spill,
         * std::function) — иначе это будет не fork, а размножение
         * непредсказуемого состояния. Только дескрипторы и exec. */
        ::close(fds[0]);
        setpgid(0, 0);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        if (fds[1] > STDERR_FILENO) ::close(fds[1]);
        execl("/bin/sh", "sh", "-c", full.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    /* Родитель. setpgid здесь — синхронизация с ребёнком, см. выше. */
    setpgid(pid, pid);
    ::close(fds[1]);

    const size_t ring_limit = opt.ring_bytes > 0 ? opt.ring_bytes : 12000u;
    const auto started = std::chrono::steady_clock::now();

    std::ofstream spill;
    if (!opt.spill_path.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(opt.spill_path).parent_path(), ec);
        spill.open(opt.spill_path, std::ios::binary | std::ios::trunc);
    }

    std::string ring;                 /* хвост вывода */
    std::string last_line;            /* для прогресса */
    char buf[8192];
    auto last_report = std::chrono::steady_clock::now();
    auto kill_deadline = std::chrono::steady_clock::time_point::max();
    bool killed = false;
    /* Срок таймаута — тот же механизм, что и отмена, и то же «5 с на
     * завершение»: иначе процесс, игнорирующий TERM, оставил бы висящий
     * worker (D1). */
    const auto timeout_at = opt.timeout_sec > 0
        ? started + std::chrono::seconds(opt.timeout_sec)
        : std::chrono::steady_clock::time_point::max();

    for (;;) {
        /* poll, а не блокирующее чтение: между чтениями обязано быть место,
         * где проверяется признак отмены. Блокирующий read ждал бы, пока
         * процесс что-нибудь напечатает, а молчащая команда (sleep, тест без
         * вывода) не печатает ничего — и «стоп» снова ждал бы её. */
        struct pollfd pfd;
        pfd.fd = fds[0];
        pfd.events = POLLIN;
        const int pr = poll(&pfd, 1, 100);
        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            const ssize_t n = ::read(fds[0], buf, sizeof(buf));
            if (n > 0) {
                stats.total_bytes += static_cast<size_t>(n);
                if (spill.is_open())
                    spill.write(buf, static_cast<std::streamsize>(n));
                ring.append(buf, static_cast<size_t>(n));
                if (ring.size() > ring_limit) {
                    /* Сдвигаем кольцо: начало уже не нужно, а конец — самое
                     * ценное. */
                    const size_t drop = ring.size() - ring_limit;
                    ring.erase(0, drop);
                    stats.dropped_bytes += drop;
                }
                size_t nl = 0;
                for (ssize_t k = 0; k < n; ++k) if (buf[k] == '\n') ++nl;
                if (nl > 0) {
                    std::string ln(buf, static_cast<size_t>(n));
                    while (!ln.empty() && (ln.back() == '\n' || ln.back() == '\r'))
                        ln.pop_back();
                    if (!ln.empty()) last_line = ln.substr(0, 120);
                }
                if (opt.on_progress) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last_report >= std::chrono::milliseconds(1000)) {
                        last_report = now;
                        opt.on_progress(stats.total_bytes, last_line);
                    }
                }
            } else if (n == 0) {
                break;   /* EOF: команда закрыла вывод */
            }
        }
        if (pr < 0 && errno != EINTR) break;

        const bool timed_out_now = !stats.cancelled && !stats.timed_out &&
                                   std::chrono::steady_clock::now() >= timeout_at;
        if (timed_out_now) stats.timed_out = true;

        if (!stats.cancelled && !stats.timed_out && opt.should_cancel &&
            opt.should_cancel()) {
            stats.cancelled = true;
        }
        /* Добиваем по достижении grace. Проверять «жива ли группа» через
         * kill(-pid, 0) не нужно: единственный читатель EOF — конец канала,
         * и он же служит признаком, что группа ушла. */
        if ((stats.cancelled || stats.timed_out) && !killed) {
            /* Первый удар: вся группа, а не только верхний sh. Минус pid —
             * группа, которую создал setpgid выше. */
            ::kill(-pid, SIGTERM);
            killed = true;
            kill_deadline = std::chrono::steady_clock::now() +
                            std::chrono::seconds(opt.kill_grace_sec);
        }
        if (killed && std::chrono::steady_clock::now() >= kill_deadline) {
            ::kill(-pid, SIGKILL);
            kill_deadline = std::chrono::steady_clock::time_point::max();
        }
    }
    ::close(fds[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}

    if (spill.is_open()) spill.close();

    stats.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
    stats.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    /* Код 124 больше не приходит от timeout(1): его здесь нет, и таймаут
     * помечается флагом выше. Осталась причина положить его на всякий
     * случай — команда может вернуть 124 сама. */
    stats.timed_out = stats.timed_out || (stats.exit_code == 124 && !stats.cancelled);
    if (!opt.spill_path.empty() && spill.good()) stats.spill_path = opt.spill_path;

    if (stats.dropped_bytes > 0) {
        std::string head = "[начало вывода утрачено: " +
                           std::to_string(stats.dropped_bytes) +
                           " байт предшествует показанному]\n";
        return head + ring;
    }
    return ring;
}

/* Каталог для spill. Один на плагин (И4.8/И4.10): полный вывод команды —
 * временный артефакт, и разбрасывать его по сессиям незачем до И5, где
 * появится настоящий session_id. */
inline std::string spill_dir(const std::string& data_dir) {
    if (data_dir.empty()) return "";
    return data_dir + "/wp_coder/trunc";
}

/* Путь для следующего spill-файла: <data_dir>/wp_coder/trunc/<tool>-<n>.log */
inline std::string next_spill_path(const std::string& data_dir,
                                   const std::string& tool) {
    const std::string dir = spill_dir(data_dir);
    if (dir.empty()) return "";
    static std::atomic<unsigned> counter{0};
    return dir + "/" + tool + "-" + std::to_string(counter.fetch_add(1)) + ".log";
}

/* Обрезать длинный вывод, сохранив маркер обрезки. */
inline std::string cap(const std::string& s, size_t limit) {
    std::string clean = text::is_valid_utf8(s) ? s : text::sanitize_utf8(s);
    if (clean.size() <= limit) return clean;
    std::string r = text::utf8_prefix(clean, limit);
    r += "\n...[вывод обрезан, продолжение недоступно]";
    return r;
}

/* Single-quote экранирование аргумента shell. */
inline std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

/* Первая строка вывода (для короткого статуса). */
inline std::string first_line(const std::string& s) {
    size_t p = s.find('\n');
    return (p == std::string::npos) ? s : s.substr(0, p);
}

} // namespace shell
} // namespace coder