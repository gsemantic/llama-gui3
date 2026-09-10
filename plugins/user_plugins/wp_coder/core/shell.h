#pragma once

/*
 * shell.h — безопасные обёртки вокруг системных команд.
 *
 * Все вызовы команд снабжаются таймаутом (timeout(1)), чтобы зависший
 * процесс не вешал worker-поток агента навсегда. Аргументы экранируются
 * через shell_quote() (single-quote), чтобы кавычки из текста модели не
 * ломали команду и не позволяли инъекцию.
 */

#include <cstdio>
#include <string>

namespace coder {
namespace shell {

/* Выполнить команду с таймаутом, вернуть объединённый stdout+stderr.
   timeout_sec == 0 — без таймаута (не рекомендуется). */
inline std::string run_capture(const std::string& cmd, unsigned timeout_sec = 60) {
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
    std::string full;
    if (timeout_sec > 0) {
        full = "timeout -k 5 " + std::to_string(timeout_sec) + " " + cmd + " 2>&1";
    } else {
        full = cmd + " 2>&1";
    }
    FILE* f = popen(full.c_str(), "r");
    out.clear();
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

/* Обрезать длинный вывод, сохранив маркер обрезки. */
inline std::string cap(const std::string& s, size_t limit) {
    if (s.size() <= limit) return s;
    std::string r = s.substr(0, limit);
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