/*
 * sudo_auth.cpp — реализация запроса пароля sudo и запуска команды под PTY.
 * Устройство и границы секрета описаны в sudo_auth.h.
 */

#include "sudo_auth.h"
#include "engine.h"
#include "shell.h"

#include <pty.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace coder {
namespace {

/* Уникальная метка приглашения. Пароль отправляется ТОЛЬКО когда этот
 * текст виден в конце вывода: у sudo свой `passprompt` задаётся флагом
 * -p, поэтому чужой вывод «password for …» не может спровоцировать
 * отправку (иначе команда, печатающая такой текст и читающая stdin,
 * получила бы пароль). */
constexpr const char* kSudoPrompt = "wp_coder_sudo_password:";

/* Хвост вывода, который показывается модели и UI — тот же предел, что у
 * shell::run_capture_stream. */
constexpr size_t kRingBytes = 12000;

void secure_wipe_impl(std::string& s) {
    if (s.empty()) return;
    volatile char* p = const_cast<volatile char*>(s.data());
    for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
    s.clear();
}

bool all_chars(const std::string& s, size_t from, bool (*pred)(int)) {
    for (size_t i = from; i < s.size(); ++i)
        if (!pred(static_cast<unsigned char>(s[i]))) return false;
    return true;
}

bool is_blank(int c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool is_star_or_blank(int c) {
    /* sudo при passwd_asterisks печатает звёздочки и backspace'ы — это
     * тоже часть приглашения, а не вывод команды. */
    return c == '*' || c == '\b' || c == 0x7f || is_blank(c);
}

/* Убрать приглашение sudo из вывода. Метка уникальна, но после неё sudo
 * может напечатать звёздочки (passwd_asterisks) — они тоже уходят, если
 * до конца строки нет ничего другого. */
void strip_prompt(std::string& out) {
    const size_t tlen = std::strlen(kSudoPrompt);
    size_t p = out.find(kSudoPrompt);
    while (p != std::string::npos) {
        const size_t e = out.find('\n', p);
        const size_t line_end = (e == std::string::npos) ? out.size() : e;
        const size_t after = p + tlen;
        if (after <= line_end && all_chars(out, after, is_star_or_blank) &&
            e != std::string::npos) {
            out.erase(p, e - p + 1);
        } else {
            out.erase(p, tlen);
        }
        p = out.find(kSudoPrompt, p);
    }
}

void replace_all(std::string& s, const std::string& from,
                 const std::string& to) {
    if (from.empty()) return;
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) {
        s.replace(p, from.size(), to);
        p += to.size();
    }
}

/* Приглашение вырезается, переводы строк PTY нормализуются, а пароль
 * (если он всё-таки отразился — чужой getpass, passwd_asterisks) на
 * всякий случай заменяется. Этот же фильтр применяется к хвосту,
 * который уходит в on_progress. */
std::string filter_output(std::string s, const std::string& password) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') continue;
        r.push_back(s[i]);
    }
    strip_prompt(r);
    if (!password.empty()) replace_all(r, password, "***");
    /* Исходная копия (до фильтра) может содержать отражённый пароль —
     * затираем её, а не отдаём наружу. */
    secure_wipe_impl(s);
    return r;
}

/* Запуск `sudo -p <метка> -v && { <команда> }` на PTY.
 *
 * Почему sudo -v, а не «вставить -S в команду модели»: команда модели
 * остаётся нетронутой (её строки проверены политикой ДО этого места), а
 * sudo получает один пароль на всю цепочку. Кэш sudo по умолчанию
 * tty-овый, и все sudo внутри одной PTY-сессии — одна аутентификация.
 *
 * Почему не pre-auth отдельным вызовом: у sudo без tty запись кэша
 * ppid-овая (по родительскому процессу), и в составной команде `sudo a &&
 * sudo b` второй sudo получил бы другой ppid и снова спросил пароль.
 * Одна PTY на всю команду решает это без оговорок.
 *
 * fd 3 — канал статуса: `printf ok >&3` выполняется ТОЛЬКО после
 * успешного `sudo -v`, поэтому «пароль принят» и «команда упала сами по
 * себе» различаются честно, а не по коду возврата sudo. */
SudoRunResult run_pty(const std::string& sudo_bin,
                      const std::string& command,
                      const std::string& password,
                      unsigned timeout_sec,
                      int prompt_timeout_ms,
                      const std::function<bool()>& cancelled,
                      const std::function<void(size_t, const std::string&)>& on_progress,
                      const std::string& spill_path) {
    SudoRunResult res;

    const std::string script =
        shell::shell_quote(sudo_bin) + " -p " + shell::shell_quote(kSudoPrompt) +
        " -v && { printf ok >&3; exec 3>&-; exec sh -c " +
        shell::shell_quote(command) + "; }";

    int status_pipe[2];
    if (pipe(status_pipe) != 0) {
        res.error = "не удалось создать канал статуса sudo";
        return res;
    }

    /* Размер терминала: forkpty с нулевым winsize отдаёт 0x0, и
     * полноэкранные программы (редакторы, ncurses-установщики) считают,
     * что терминала нет. 80x24 — безопасный минимум, который не ломает
     * обычный вывод. */
    struct winsize ws;
    ws.ws_row = 24;
    ws.ws_col = 80;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;

    int master = -1;
    const pid_t pid = forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) {
        ::close(status_pipe[0]);
        ::close(status_pipe[1]);
        res.error = "не удалось создать терминал для sudo";
        return res;
    }
    if (pid == 0) {
        /* Ребёнок: только дескрипторы и exec. Объекты родителя (строки,
         * std::function, мьютексы) не трогаем — после fork это не fork, а
         * размножение состояния. */
        ::close(status_pipe[0]);
        /* master принадлежит родителю: если ребёнок оставит его открытым,
         * EOF на чтении придёт только после его смерти, а до неё родитель
         * будет читать пустоту вместо нормального завершения. */
        ::close(master);
        if (status_pipe[1] != 3) {
            dup2(status_pipe[1], 3);
            ::close(status_pipe[1]);
        }
        /* Маска сигналов и SIGPIPE наследуются от потока, который сделал
         * fork: если SIGPIPE заблокирован, sudo/команда получат EPIPE
         * вместо нормального поведения. */
        sigset_t empty;
        sigemptyset(&empty);
        pthread_sigmask(SIG_SETMASK, &empty, nullptr);
        signal(SIGPIPE, SIG_DFL);
        execl("/bin/sh", "sh", "-c", script.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }

    ::close(status_pipe[1]);
    const int fl = fcntl(master, F_GETFL, 0);
    if (fl >= 0) fcntl(master, F_SETFL, fl | O_NONBLOCK);

    std::ofstream spill;
    bool spill_ok = false;
    if (!spill_path.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(spill_path).parent_path(), ec);
        spill.open(spill_path, std::ios::binary | std::ios::trunc);
        spill_ok = spill.is_open();
    }

    const auto started = std::chrono::steady_clock::now();
    const auto auth_deadline =
        started + std::chrono::milliseconds(prompt_timeout_ms > 0
                                                ? prompt_timeout_ms
                                                : 300000);
    auto cmd_deadline = std::chrono::steady_clock::time_point::max();

    std::string out;
    size_t total = 0;
    size_t dropped = 0;
    size_t ring_limit = kRingBytes;
    char buf[8192];
    std::string last_line;
    auto last_report = started;

    bool sent = false;
    bool rejected = false;
    bool auth_ok = false;
    bool timed_out = false;
    bool was_cancelled = false;
    bool killed = false;
    bool master_open = true;
    bool status_open = true;
    bool reaped = false;
    int exit_code = -1;
    auto kill_deadline = std::chrono::steady_clock::time_point::max();

    auto kill_group = [&]() {
        if (killed) return;
        ::kill(-pid, SIGTERM);
        killed = true;
        kill_deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(3);
    };

    while (master_open || status_open) {
        struct pollfd pfds[2];
        int nfds = 0;
        int mslot = -1, sslot = -1;
        if (master_open) {
            mslot = nfds;
            pfds[nfds].fd = master;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            ++nfds;
        }
        if (status_open) {
            sslot = nfds;
            pfds[nfds].fd = status_pipe[0];
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            ++nfds;
        }
        if (nfds == 0) break;

        const int pr = poll(pfds, nfds, 100);
        if (pr < 0 && errno != EINTR) break;

        if (mslot >= 0 && (pfds[mslot].revents & (POLLIN | POLLHUP))) {
            const ssize_t n = ::read(master, buf, sizeof(buf));
            if (n > 0) {
                total += static_cast<size_t>(n);
                if (spill.is_open())
                    spill.write(buf, static_cast<std::streamsize>(n));
                out.append(buf, static_cast<size_t>(n));
                if (out.size() > ring_limit) {
                    const size_t drop = out.size() - ring_limit;
                    out.erase(0, drop);
                    dropped += drop;
                }
                size_t nl = 0;
                for (ssize_t k = 0; k < n; ++k) if (buf[k] == '\n') ++nl;
                if (nl > 0) {
                    std::string ln(buf, static_cast<size_t>(n));
                    while (!ln.empty() && (ln.back() == '\n' || ln.back() == '\r'))
                        ln.pop_back();
                    if (!ln.empty()) last_line = ln.substr(0, 120);
                    secure_wipe_impl(ln);
                }

                /* Приглашение: метка должна быть в ХВОСТЕ вывода (после
                 * неё только пробелы/перевод строки). Иначе это упоминание
                 * в тексте команды, и пароль туда уходить не должен. */
                {
                    const size_t p = out.rfind(kSudoPrompt);
                    if (p != std::string::npos) {
                        size_t e = p + std::strlen(kSudoPrompt);
                        while (e < out.size() && is_blank(static_cast<unsigned char>(out[e]))) ++e;
                        if (e == out.size() && sent) {
                            /* sudo показал приглашение ВТОРОЙ раз: пароль
                             * отвергнут. Второй пароль не отправляем —
                             * это уже перебор, а не ввод; быстро завершаем,
                             * чтобы человек получил «не принят» и окно
                             * повтора, а не ждал пять минут. */
                            rejected = true;
                            kill_group();
                        } else if (e == out.size()) {
                            std::string pw = password + "\n";
                            size_t off = 0;
                            /* SIGPIPE заблокирован только вокруг записи: если
                             * ребёнок уже ушёл, write вернёт EPIPE, а не
                             * убьёт процесс приложения. */
                            sigset_t block, old;
                            sigemptyset(&block);
                            sigaddset(&block, SIGPIPE);
                            pthread_sigmask(SIG_BLOCK, &block, &old);
                            while (off < pw.size()) {
                                const ssize_t w = ::write(master, pw.data() + off,
                                                          pw.size() - off);
                                if (w > 0) { off += static_cast<size_t>(w); continue; }
                                if (w < 0 && errno == EINTR) continue;
                                break;
                            }
                            pthread_sigmask(SIG_SETMASK, &old, nullptr);
                            secure_wipe_impl(pw);
                            sent = true;
                        }
                    }
                }

                if (on_progress) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last_report >= std::chrono::milliseconds(1000)) {
                        last_report = now;
                        /* Хвост уходит в чат приложения, поэтому фильтр
                         * тот же: приглашение и пароль (если чужой getpass
                         * его отразил) в него попасть не должны. */
                        on_progress(total, filter_output(last_line, password));
                    }
                }
            } else if (n == 0) {
                master_open = false;
            } else if (errno != EIO && errno != EINTR) {
                master_open = false;
            }
        }

        if (sslot >= 0 && (pfds[sslot].revents & (POLLIN | POLLHUP))) {
            char sbuf[64];
            const ssize_t n = ::read(status_pipe[0], sbuf, sizeof(sbuf));
            if (n > 0) {
                if (std::string(sbuf, static_cast<size_t>(n)).find("ok") !=
                    std::string::npos) {
                    auth_ok = true;
                    status_open = false;
                    ::close(status_pipe[0]);
                    /* Таймаут команды считается от момента, когда пароль
                     * принят: ожидание человека не должно съедать бюджет
                     * самой команды. */
                    if (timeout_sec > 0) {
                        cmd_deadline = std::chrono::steady_clock::now() +
                                       std::chrono::seconds(timeout_sec);
                    }
                }
            } else {
                status_open = false;
                ::close(status_pipe[0]);
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (!auth_ok && now >= auth_deadline) {
            timed_out = true;
            kill_group();
        }
        if (auth_ok && timeout_sec > 0 && now >= cmd_deadline) {
            timed_out = true;
            kill_group();
        }
        if (!was_cancelled && cancelled && cancelled()) {
            was_cancelled = true;
            kill_group();
        }
        if (killed && now >= kill_deadline) {
            ::kill(-pid, SIGKILL);
            kill_deadline = std::chrono::steady_clock::time_point::max();
        }

        if (!reaped) {
            int st = 0;
            const pid_t done = waitpid(pid, &st, WNOHANG);
            if (done == pid) {
                reaped = true;
                exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
                /* Дерево могло оставить вывод в канале: один добор после
                 * смерти, чтобы хвост не потерялся. */
                for (;;) {
                    const ssize_t n = ::read(master, buf, sizeof(buf));
                    if (n <= 0) break;
                    total += static_cast<size_t>(n);
                    if (spill.is_open())
                        spill.write(buf, static_cast<std::streamsize>(n));
                    out.append(buf, static_cast<size_t>(n));
                    if (out.size() > ring_limit) {
                        const size_t drop = out.size() - ring_limit;
                        out.erase(0, drop);
                        dropped += drop;
                    }
                }
                break;
            }
        }
    }

    if (!reaped) {
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    }
    if (master_open) ::close(master);
    if (spill.is_open()) spill.close();

    res.total_bytes = total;
    res.dropped_bytes = dropped;
    res.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - started)
                         .count();
    res.exit_code = exit_code;
    res.timed_out = timed_out;
    res.cancelled = was_cancelled;
    res.output = filter_output(out, password);
    secure_wipe_impl(out);
    secure_wipe_impl(last_line);
    volatile char* vbuf = reinterpret_cast<volatile char*>(buf);
    for (size_t i = 0; i < sizeof(buf); ++i) vbuf[i] = 0;
    if (spill_ok) res.spill_path = spill_path;
    res.executed = auth_ok;
    res.prompt_seen = sent;
    if (!auth_ok) {
        if (was_cancelled)
            res.error = "ввод пароля sudo отменён пользователем";
        else if (timed_out)
            res.error = "пользователь не ввёл пароль sudo";
        else if (rejected)
            res.error = "пароль sudo не принят";
        else if (sent)
            res.error = "пароль sudo не принят";
        else
            res.error = "sudo не запросил пароль (не установлен, либо пользователь"
                        " не в sudoers)";
    }
    return res;
}

} // namespace

void secure_wipe(std::string& s) { secure_wipe_impl(s); }

void SudoAuth::bind(EngineState* state,
                    std::function<void(const std::string&)> push) {
    state_ = state;
    push_ = std::move(push);
}

SudoAuth::AskResult SudoAuth::ask(const std::string& command,
                                  const std::string& error) {
    AskResult res;
    SudoRequest req;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (cancelled_) {
            res.cancelled = true;
            return res;
        }
        req.id = ++next_id_;
        req.command = command;
        req.error = error;
        pending_.push_back(req);
    }

    AgentState prev = AgentState::Executing;
    if (state_) {
        std::lock_guard<std::mutex> lk(state_->mtx);
        prev = state_->state;
        state_->state = AgentState::WaitingPermission;
    }
    if (push_) {
        std::string cmd = command.size() > 160 ? command.substr(0, 160) + "…" : command;
        push_("[sudo] требуется пароль для команды: " + cmd);
    }

    bool timed_out = false;
    {
        std::unique_lock<std::mutex> lk(mtx_);
        auto decided = [&]() {
            for (const auto& r : pending_)
                if (r.id == req.id) return r.decided;
            return true;   /* снят из очереди (cancel_all) */
        };
        if (wait_timeout_ms_ > 0) {
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(wait_timeout_ms_);
            while (!decided() && !cancelled_) {
                if (cv_.wait_until(lk, deadline) == std::cv_status::timeout) {
                    timed_out = true;
                    break;
                }
            }
        } else {
            cv_.wait(lk, [&]() { return decided() || cancelled_; });
        }

        for (auto& r : pending_) {
            if (r.id != req.id) continue;
            res.granted = r.allowed;
            res.password = std::move(r.password);
            secure_wipe_impl(r.password);
            break;
        }
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                      [&req](const SudoRequest& r) {
                                          return r.id == req.id;
                                      }),
                       pending_.end());
        if (cancelled_) res.cancelled = true;
    }

    if (state_) {
        std::lock_guard<std::mutex> lk(state_->mtx);
        if (state_->state == AgentState::WaitingPermission)
            state_->state = prev;
    }

    if (push_) {
        if (res.granted)
            push_("[sudo] пароль получен");
        else if (res.cancelled)
            push_("[sudo] ввод пароля отменён");
        else if (timed_out)
            push_("[sudo] пароль не введён");
        else
            push_("[sudo] ввод пароля отклонён");
    }
    return res;
}

SudoRunResult SudoAuth::run(const std::string& command, unsigned timeout_sec,
                            const std::function<bool()>& cancelled,
                            const std::function<void(size_t, const std::string&)>& on_progress,
                            const std::string& spill_path) {
    SudoRunResult res;
    std::string last_error;

    for (int attempt = 1; attempt <= max_attempts_; ++attempt) {
        if (cancelled && cancelled()) {
            res.cancelled = true;
            res.error = "отменено пользователем";
            return res;
        }
        std::string ask_error;
        if (attempt > 1) {
            ask_error = "Пароль не принят. Попытка " +
                        std::to_string(attempt) + " из " +
                        std::to_string(max_attempts_) + ".";
        }
        AskResult a = ask(command, ask_error);
        if (!a.granted) {
            res.cancelled = a.cancelled;
            res.timed_out = a.timed_out;
            res.error = a.cancelled ? "ввод пароля sudo отменён"
                      : a.timed_out ? "пароль sudo не введён"
                                    : "ввод пароля sudo отклонён";
            return res;
        }

        res = run_pty(sudo_bin_, command, a.password, timeout_sec,
                      prompt_timeout_ms_, cancelled, on_progress, spill_path);
        /* Пароль больше не нужен ни здесь, ни в копии запроса. */
        secure_wipe(a.password);
        if (res.executed) return res;
        if (res.cancelled || res.timed_out) return res;
        /* sudo не показал приглашение (не установлен, отказ PAM до
         * запроса пароля) — повторный вопрос человеку бессмысленен. */
        if (!res.prompt_seen) return res;
        last_error = res.error;
    }

    /* Копия последней попытки, а не пустая структура: prompt_seen и
     * признак отмены нужны вызывающему, чтобы отличить «пароль не
     * принят» от «sudo не установлен» и не задавать вопрос повторно. */
    SudoRunResult out = res;
    out.executed = false;
    out.error = "пароль sudo не принят после " +
                std::to_string(max_attempts_) + " попыток";
    if (!last_error.empty()) out.error += " (" + last_error + ")";
    return out;
}

bool SudoAuth::reply(uint64_t id, const std::string& password) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& r : pending_) {
            if (r.id != id || r.decided) continue;
            r.password = password;
            r.decided = true;
            r.allowed = true;
            cv_.notify_all();
            return true;
        }
    }
    return false;
}

bool SudoAuth::reply(uint64_t id, const char* password) {
    if (!password) return false;
    std::string pw(password);
    const bool ok = reply(id, pw);
    secure_wipe_impl(pw);
    return ok;
}

bool SudoAuth::reject(uint64_t id) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& r : pending_) {
            if (r.id != id || r.decided) continue;
            r.decided = true;
            r.allowed = false;
            cv_.notify_all();
            return true;
        }
    }
    return false;
}

std::vector<SudoRequest> SudoAuth::pending() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<SudoRequest> copy = pending_;
    /* Копия для окна не должна нести секрет: окно рисует id, команду и
     * ошибку, а пароль уже лежит в этом векторе только потому, что
     * std::vector хранит его вместе с остальными полями. */
    for (auto& r : copy) {
        secure_wipe_impl(r.password);
        r.password.shrink_to_fit();
    }
    return copy;
}

size_t SudoAuth::pending_count() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return pending_.size();
}

void SudoAuth::cancel_all() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        cancelled_ = true;
        for (auto& r : pending_) secure_wipe_impl(r.password);
        pending_.clear();
    }
    cv_.notify_all();
}

void SudoAuth::clear_cancel() {
    std::lock_guard<std::mutex> lk(mtx_);
    cancelled_ = false;
}

void SudoAuth::set_wait_timeout_ms(int ms) {
    std::lock_guard<std::mutex> lk(mtx_);
    wait_timeout_ms_ = ms;
}

int SudoAuth::wait_timeout_ms() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return wait_timeout_ms_;
}

void SudoAuth::set_max_attempts(int n) {
    std::lock_guard<std::mutex> lk(mtx_);
    max_attempts_ = n > 0 ? n : 1;
}

int SudoAuth::max_attempts() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return max_attempts_;
}

void SudoAuth::set_prompt_timeout_ms(int ms) {
    std::lock_guard<std::mutex> lk(mtx_);
    prompt_timeout_ms_ = ms > 0 ? ms : 1;
}

int SudoAuth::prompt_timeout_ms() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return prompt_timeout_ms_;
}

void SudoAuth::set_sudo_binary(std::string path) {
    std::lock_guard<std::mutex> lk(mtx_);
    sudo_bin_ = std::move(path);
}

std::string SudoAuth::sudo_binary() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return sudo_bin_;
}

} // namespace coder