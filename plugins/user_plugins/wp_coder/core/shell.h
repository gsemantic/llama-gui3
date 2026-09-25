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
 * exec_command проверяет свою команду сам и строже (allowlist программ и
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