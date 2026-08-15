#include "headless.h"

#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

namespace news_rewriter {

namespace {

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

// Число «видимых» букв в HTML (без <script>/<style> и тегов). Латиница + любые
// не-ASCII байты (в UTF-8 это и есть кириллица/прочее). Используется, чтобы
// отличить пустую JS-оболочку (единицы букв) от реальной страницы (тысячи).
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

} // namespace

bool HeadlessRenderer::resolve_browser(const NetworkConfig& cfg,
                                       std::string& out_path) const {
    const std::string bp = cfg.browser_path.empty() ? "chromium" : cfg.browser_path;
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

bool HeadlessRenderer::available(const NetworkConfig& cfg) const {
    std::string path;
    return resolve_browser(cfg, path);
}

bool HeadlessRenderer::is_thin_content(const std::string& html) const {
    // Меньше ~200 видимых букв — признак JS-оболочки (реальные статьи содержат
    // тысячи). Оценка намеренно грубая.
    return visible_letter_count(html) < 200;
}

std::string HeadlessRenderer::render(const std::string& url,
                                     const NetworkConfig& cfg,
                                     std::string* error) {
    std::string browser;
    if (!resolve_browser(cfg, browser)) {
        if (error) {
            *error = "браузер не найден: " +
                     (cfg.browser_path.empty() ? std::string("chromium")
                                               : cfg.browser_path);
        }
        return "";
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        if (error) *error = "pipe() не удался";
        return "";
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        if (error) *error = "fork() не удался";
        return "";
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
        const std::string ua =
            cfg.user_agent.empty() ? std::string(kDefaultUserAgent) : cfg.user_agent;
        const std::string vtb = "--virtual-time-budget=15000";
        const std::vector<std::string> args = {
            browser,
            "--headless",
            "--no-sandbox",
            "--disable-gpu",
            "--disable-dev-shm-usage",
            "--ignore-certificate-errors",
            "--user-agent=" + ua,
            vtb,
            "--dump-dom",
            url,
        };
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(browser.c_str(), argv.data());
        _exit(127);  // execvp не вернулся — браузер не запустился
    }

    // Родитель: читаем DOM из трубы с учётом предельного времени (wall-clock).
    close(pipefd[1]);
    const int timeout_ms = cfg.headless_timeout_ms > 0 ? cfg.headless_timeout_ms : 30000;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    std::string out;
    constexpr std::size_t kMaxBytes = 32 * 1024 * 1024;  // защита от переполнения
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
        if (sel <= 0) {
            kill(pid, SIGKILL);
            break;
        }
        const ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n <= 0) break;  // EOF или ошибка
        if (out.size() + static_cast<std::size_t>(n) > kMaxBytes) {
            kill(pid, SIGKILL);
            break;
        }
        out.append(buf, static_cast<std::size_t>(n));
    }
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    if (out.empty()) {
        if (error) *error = "браузер не вернул DOM (возможно, истекло время рендера)";
        return "";
    }
    return out;
}

} // namespace news_rewriter
