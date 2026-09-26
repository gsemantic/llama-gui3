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
#include <sys/wait.h>

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
};

inline std::string run_capture_stream(const std::string& cmd,
                                      const RunOptions& opt, RunStats& stats) {
    stats = RunStats();
    std::string refusal = security::check_assembled_command(cmd);
    if (!refusal.empty()) return "[запрещено политикой команд] " + refusal;

    /* Таймаут считает и обрыв: -k 5 означает «дать 5 с на завершение
     * после SIGTERM», иначе процесс, игнорирующий TERM, остался бы
     * висеть навсегда — то есть висящий GUI (D1). */
    std::string full;
    if (opt.timeout_sec > 0)
        full = "timeout -k 5 " + std::to_string(opt.timeout_sec) + " " + cmd + " 2>&1";
    else
        full = cmd + " 2>&1";

    const size_t ring_limit = opt.ring_bytes > 0 ? opt.ring_bytes : 12000u;
    const auto started = std::chrono::steady_clock::now();
    FILE* f = popen(full.c_str(), "r");
    if (!f) return "[ошибка] не удалось запустить: " + cmd;

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
    while (fgets(buf, sizeof(buf), f)) {
        const size_t n = std::strlen(buf);
        stats.total_bytes += n;
        if (spill.is_open()) spill.write(buf, static_cast<std::streamsize>(n));
        ring += buf;
        if (ring.size() > ring_limit) {
            /* Сдвигаем кольцо: начало уже не нужно, а конец — самое
             * ценное. */
            const size_t drop = ring.size() - ring_limit;
            ring.erase(0, drop);
            stats.dropped_bytes += drop;
        }
        if (n > 1) {
            std::string ln(buf, n);
            while (!ln.empty() && (ln.back() == '\n' || ln.back() == '\r')) ln.pop_back();
            if (!ln.empty()) last_line = ln.substr(0, 120);
        }
        if (opt.on_progress) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_report >= std::chrono::milliseconds(1000)) {
                last_report = now;
                opt.on_progress(stats.total_bytes, last_line);
            }
        }
    }
    /* pclose отдаёт статус ожидания, а не код возврата: без WEXITSTATUS он
     * всегда был нулевым, и таймаут выглядел как успех (это поймал тест
     * на таймаут — первая версия писала WEXITSTATUS(0)). */
    const int status = pclose(f);
    if (spill.is_open()) spill.close();

    stats.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
    stats.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    /* 124 — код возврата timeout(1): команда не уложилась. Сама команда
     * тоже может вернуть 124, и это неразличимо — но ложный «таймаут»
     * честнее, чем ложное «успех». */
    stats.timed_out = (stats.exit_code == 124);
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