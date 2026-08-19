#include "headless_browser/headless_browser.h"

#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <cerrno>

namespace headless_browser {
namespace {

// Дефолтный User-Agent — реальный браузер (Chromium), чтобы сайты не блокировали
// headless-браузер как бота (некоторые отдают 302/челлендж для пустого UA).
const char* kDefaultUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 YaBrowser/24.7.0.0 "
    "Yowser/2.5 Safari/537.36";

// Разрешает browser_path в абсолютный путь к исполняемому файлу (поиск в PATH
// для имени без слэшей). Возвращает false, если файл недоступен.
bool resolve_browser(const RenderOptions& opts, std::string& out_path) {
    const std::string bp = opts.browser_path.empty() ? "chromium" : opts.browser_path;
    if (bp.find('/') != std::string::npos) {
        if (access(bp.c_str(), X_OK) == 0) {
            out_path = bp;
            return true;
        }
        return false;
    }
    const char* penv = std::getenv("PATH");
    if (!penv) return false;
    const std::string paths = penv;
    std::size_t start = 0;
    while (start <= paths.size()) {
        const std::size_t col = paths.find(':', start);
        const std::string dir =
            paths.substr(start, col == std::string::npos ? std::string::npos : col - start);
        const std::string cand = dir + "/" + bp;
        if (access(cand.c_str(), X_OK) == 0) {
            out_path = cand;
            return true;
        }
        if (col == std::string::npos) break;
        start = col + 1;
    }
    return false;
}

// Запускает браузер с заданными аргументами и захватывает его stdout (в out).
// Возвращает true, если процесс завершился и что-то выдал; при превышении
// wall-clock таймаута — убивает процесс и возвращает false.
bool spawn_and_capture(const std::string& browser,
                        const std::vector<std::string>& args,
                        int timeout_ms, std::size_t max_output_bytes,
                        std::string& out, std::string* error,
                        bool require_output = true) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        if (error) *error = "pipe() не удался";
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        if (error) *error = "fork() не удался";
        return false;
    }

    if (pid == 0) {
        // Дочерний процесс: stdout → труба, stderr → /dev/null, запуск браузера.
        dup2(pipefd[1], 1);
        close(pipefd[0]);
        close(pipefd[1]);
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, 2);
            close(devnull);
        }
        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        // argv[0] должен быть именем исполняемого файла (как требует execvp и
        // ожидают программы вроде Chromium), иначе аргументы «съезжают».
        argv.push_back(const_cast<char*>(browser.c_str()));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(browser.c_str(), argv.data());
        _exit(127);  // execvp не вернулся — браузер не запустился
    }

    // Родитель: читаем stdout с учётом предельного времени (wall-clock).
    close(pipefd[1]);
    const auto deadline = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 30000);
    char buf[4096];
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const int remain_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (remain_ms <= 0) {
            kill(pid, SIGKILL);
            break;
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pipefd[0], &rfds);
        struct timeval tv;
        tv.tv_sec = remain_ms / 1000;
        tv.tv_usec = (remain_ms % 1000) * 1000;
        const int sel = select(pipefd[0] + 1, &rfds, nullptr, nullptr, &tv);
        if (sel < 0) {
            // EINTR: select прерван сигналом (напр. SIGCHLD от дочерних
            // процессов Chromium) — это не сбой, просто повторяем ожидание.
            if (errno == EINTR) continue;
            kill(pid, SIGKILL);
            break;
        }
        if (sel == 0) {
            kill(pid, SIGKILL);  // истёк предельный таймаут
            break;
        }
        const ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;  // прерывание — повторяем чтение
            break;
        }
        if (n == 0) break;  // EOF
        if (out.size() + static_cast<std::size_t>(n) > max_output_bytes) {
            // Превышен лимит вывода — отсекаем лишнее и убиваем процесс.
            const std::size_t room = max_output_bytes - out.size();
            out.append(buf, room);
            kill(pid, SIGKILL);
            break;
        }
        out.append(buf, static_cast<std::size_t>(n));
    }
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (require_output && out.empty()) {
        if (error) *error = "браузер не вернул данных (возможно, истекло время)";
        return false;
    }
    return true;
}

// Удаляет из строки блоки <tag ...>...</tag> (script/style), не трогая вложенность
// имена. Достаточно грубо, но для оценки «видимого» текста хватает.
void strip_blocks(std::string& t, const char* tag) {
    const std::string open = std::string("<") + tag;
    const std::string close = std::string("</") + tag + ">";
    std::size_t pos = 0;
    while ((pos = t.find(open, pos)) != std::string::npos) {
        const std::size_t body = t.find('>', pos);
        if (body == std::string::npos) break;
        const std::size_t end = t.find(close, body);
        if (end == std::string::npos) break;
        t.erase(pos, end + close.size() - pos);
    }
}

} // namespace

std::size_t visible_letter_count(const std::string& html) {
    std::string s = html;
    strip_blocks(s, "script");
    strip_blocks(s, "style");
    std::string text;
    bool intag = false;
    for (char c : s) {
        if (c == '<') intag = true;
        else if (c == '>') intag = false;
        else if (!intag) text.push_back(c);
    }
    std::size_t n = 0;
    for (unsigned char c : text) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c > 127) n++;
    }
    return n;
}

bool is_thin_content(const std::string& html, std::size_t threshold) {
    return visible_letter_count(html) < threshold;
}

namespace {

std::vector<std::string> base_args(const RenderOptions& opts, const std::string& url) {
    const std::string ua =
        opts.user_agent.empty() ? std::string(kDefaultUserAgent) : opts.user_agent;
    return {
        "--headless",
        "--no-sandbox",
        "--disable-gpu",
        "--disable-dev-shm-usage",
        "--ignore-certificate-errors",
        "--user-agent=" + ua,
        "--virtual-time-budget=" + std::to_string(opts.virtual_time_budget_ms),
        url,
    };
}

} // namespace

bool available(const RenderOptions& opts) {
    std::string path;
    return resolve_browser(opts, path);
}

std::string render_dom(const std::string& url, const RenderOptions& opts,
                       std::string* error) {
    std::string browser;
    if (!resolve_browser(opts, browser)) {
        if (error) {
            *error = "браузер не найден: " +
                     (opts.browser_path.empty() ? std::string("chromium")
                                                : opts.browser_path);
        }
        return "";
    }

    std::vector<std::string> args = base_args(opts, url);
    args.push_back("--dump-dom");

    std::string out;
    if (!spawn_and_capture(browser, args, opts.timeout_ms,
                           static_cast<std::size_t>(opts.max_output_bytes), out,
                           error)) {
        return "";
    }
    return out;
}

bool screenshot(const std::string& url, const std::string& out_path,
                const RenderOptions& opts, std::string* error) {
    std::string browser;
    if (!resolve_browser(opts, browser)) {
        if (error) {
            *error = "браузер не найден: " +
                     (opts.browser_path.empty() ? std::string("chromium")
                                                : opts.browser_path);
        }
        return false;
    }

    std::vector<std::string> args = base_args(opts, url);
    args.push_back("--screenshot=" + out_path);
    args.push_back("--window-size=" + std::to_string(opts.window_width) + "," +
                   std::to_string(opts.window_height));

    std::string out;
    if (!spawn_and_capture(browser, args, opts.timeout_ms,
                           static_cast<std::size_t>(opts.max_output_bytes), out,
                           error, /*require_output=*/false)) {
        return false;
    }
    // Chromium пишет PNG в out_path; проверяем, что файл появился и не пустой.
    struct stat st {};
    if (stat(out_path.c_str(), &st) != 0 || st.st_size == 0) {
        if (error) *error = "скриншот не создан (файл отсутствует или пустой)";
        return false;
    }
    return true;
}

} // namespace headless_browser
