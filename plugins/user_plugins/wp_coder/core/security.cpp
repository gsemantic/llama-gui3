#include "security.h"

#include "command_policy.h"

#include <algorithm>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace coder {
namespace security {

bool is_path_safe(const std::string& path) {
    /* Проверяем на path traversal. */
    if (path.find("..") != std::string::npos) return false;
    /* Проверяем на null bytes. */
    if (path.find('\0') != std::string::npos) return false;
    return true;
}

bool is_path_outside(const std::string& abs_path,
                     const std::string& project_dir) {
    if (project_dir.empty()) return false;

    std::error_code ec;
    const fs::path root = fs::weakly_canonical(fs::path(project_dir), ec);
    if (ec) return true;
    const fs::path cand = fs::weakly_canonical(fs::path(abs_path), ec);
    if (ec) return true;

/* Ровно тот же путь — внутри.
     *
     * Строка нужна НЕ потому, что без неё неверно, а потому, что «свой
     * путь» и «разные корни» у `lexically_relative` дают ОДИН и тот же
     * признак — пустой путь, — и читать его надо по-разному. Стандарт
     * говорит, что путь относительно самого себя пуст; libstdc++ на этой
     * машине возвращает «.», то есть проверка ниже случайно даёт тот же
     * ответ и без этой строки. Мутация, снимавшая строку, ВЫЖИЛА именно
     * поэтому, и по одному toolchain'у судить нельзя: на другой реализации
     * пустой путь означал бы «вне проекта» для САМОГО корня, и каждый вызов
     * инструмента по пути в корень спрашивал бы разрешение. */
    if (cand == root) return false;

    /* `lexically_relative`, а не `fs::relative`: обе стороны уже
     * канонизированы, а `fs::relative` канонизировал бы их второй раз —
     * лишние обращения к ФС на каждый вызов инструмента. Результат тот
     * же, и это ровно та пара функций, на которых стоял эталон
     * (`WPFileAgent::is_path_in_wp_content`).
     *
     * Пустой относительный путь — это «своего не существует»: разные корни
     * (разные диски на Windows) либо пустой проверяемый путь. И то и другое
     * — снаружи. Отдельная проверка разных корней (`root_name()`) была
     * здесь и удалена: она давала ровно тот же ответ, что и эта строка, то
     * есть была кодом под признак, которого на Linux не бывает.
     *
     * Строка стоит ДО разыменования `rel.begin()` ещё и по той причине,
     * что разыменовать конец пути нельзя. */
    const fs::path rel = cand.lexically_relative(root);
    if (rel.empty()) return true;
    if (*rel.begin() == fs::path("..")) return true;
    return false;
}

bool is_path_allowed(const std::string& abs_path,
                     const std::string& project_dir,
                     const std::vector<std::string>& allowed) {
    if (!is_path_outside(abs_path, project_dir)) return true;
    for (const auto& pattern : allowed) {
        if (!pattern.empty() && pattern.back() == '*') {
            if (abs_path.find(pattern.substr(0, pattern.size() - 1)) == 0) return true;
        } else {
            if (abs_path == pattern) return true;
        }
    }
    return false;
}

bool is_project_dir_valid(const std::string& project_dir) {
    if (project_dir.empty()) return true;
    std::string normalized = project_dir;
    while (!normalized.empty() && normalized.back() == '/')
        normalized.pop_back();
    if (normalized.empty()) normalized = "/";
    /* Запрещаем только ТОЧНЫЕ пути к критическим директориям. */
    static const std::vector<std::string> forbidden = {
        "/",
        "/bin",
        "/boot",
        "/dev",
        "/etc",
        "/lib",
        "/lib64",
        "/proc",
        "/root",
        "/sbin",
        "/sys",
        "/usr",
    };
    for (const auto& f : forbidden) {
        if (normalized == f) return false;
    }
    /* /var и /usr — разрешаем подкаталоги (например /var/www/). */
    return true;
}

bool is_path_not_dangerous(const std::string& abs_path) {
    /* Не разрешаем запись в критические системные директории. */
    static const std::vector<std::string> forbidden = {
        "/etc/passwd",
        "/etc/shadow",
        "/etc/sudoers",
        "/boot/",
        "/proc/",
        "/sys/",
        "/dev/"
    };
    for (const auto& f : forbidden) {
        if (abs_path.find(f) == 0) return false;
    }
    return true;
}

const std::vector<std::string>& blocked_commands() {
    /* И3: curl и wget убраны из списка. Запрет «любой curl подстрокой» —
     * это ровно тот приём, которым был заменён allowlist: нельзя
     * перечислить плохие строки, можно перечислить допустимые хосты.
     * Теперь curl проверяется политикой команд (allowlist хостов +
     * запрет --upload-file/--config/выгрузки в системный каталог).
     * Остальные семь строк остаются как грубый фильтр: он ловит и то,
     * что разбор команды не понял. */
    static const std::vector<std::string> blocked = {
        "rm -rf /",
        "rm -rf /*",
        "mkfs",
        "dd if=",
        "> /dev/sda",
        ":(){ :|:& };:",  // fork bomb
        "chmod -R 777 /",
    };
    return blocked;
}

bool is_command_allowed(const std::string& cmd) {
    for (const auto& b : blocked_commands()) {
        if (cmd.find(b) != std::string::npos) return false;
    }
    return true;
}

std::string check_command(const std::string& cmd) {
    /* Грубый фильтр по подстрокам — первая линия (ловит то, что разбор
     * команды не понял), решение принимает политика. */
    for (const auto& b : blocked_commands()) {
        if (cmd.find(b) != std::string::npos) {
            return "команда содержит запрещённый шаблон: " + b;
        }
    }
    return command_policy().check(cmd);
}

std::string check_assembled_command(const std::string& cmd) {
    /* Без подстрочного фильтра: команду собрал код плагина, и совпадение
     * шаблона означало бы ложный отказ исправной работы инструмента. */
    return command_policy().check_assembled(cmd);
}

void trust_command_hosts(const std::vector<std::string>& hosts) {
    for (const auto& h : hosts) {
        std::string host = CommandPolicy::host_from_url(h);
        if (!host.empty()) command_policy().trust_host(host);
    }
}

std::string shell_escape(const std::string& s) {
    std::ostringstream out;
    out << "'";
    for (char c : s) {
        if (c == '\'') out << "'\\''";
        else out << c;
    }
    out << "'";
    return out.str();
}

std::string sanitize_ident(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')
            r += c;
    }
    return r;
}

bool is_shell_arg_safe(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        switch (c) {
            case ';': case '|': case '&': case '`':
            case '>': case '<': case '$':
                /* Разрешаем $$ (PID) только в начале или после пробела. */
                if (c == '$' && i + 1 < s.size() && s[i + 1] == '(')
                    return false;
                if (c != '$') return false;
                break;
            default:
                break;
        }
    }
    return true;
}

} // namespace security
} // namespace coder
