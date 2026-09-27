#include "command_policy.h"

#include <algorithm>
#include <cctype>

namespace coder {

/* ======================================================================
 * ParsedCommand
 * ====================================================================== */

std::string ParsedCommand::text() const {
    std::string t = binary;
    for (const auto& a : args) t += " " + a;
    for (const auto& r : redirections) t += " " + r;
    return t;
}

std::string ParsedCommand::program() const {
    /* Только каталог: плагин запускает '/usr/bin/php8.1' и
     * '/usr/local/bin/composer'. Номер версии и расширение убираются не
     * здесь, а в variants() — иначе имя обрезалось бы посередине. */
    std::string n = binary;
    size_t slash = n.find_last_of('/');
    if (slash != std::string::npos) n = n.substr(slash + 1);
    return n;
}

std::vector<std::string> ParsedCommand::variants() const {
    std::vector<std::string> v;
    const std::string n = program();
    if (n.empty()) return v;
    v.push_back(n);

    /* Без расширения: mkfs.ext4 → mkfs, deploy.sh → deploy. */
    size_t dot = n.find('.');
    if (dot != std::string::npos && dot > 0) v.push_back(n.substr(0, dot));

    /* Без номера версии: php8.1 → php8 → php, python3 → python.
     * Только если имя начинается с буквы: «2to3» не должен превратиться
     * в «2to», а «docker-compose» не трогаем — там хвост не цифровой. */
    if (std::isalpha(static_cast<unsigned char>(n[0]))) {
        std::string cur = n;
        for (size_t guard = 0; guard < 4; ++guard) {
            size_t end = cur.size();
            while (end > 1) {
                char c = cur[end - 1];
                if (std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '-')
                    --end;
                else
                    break;
            }
            if (end >= cur.size()) break;
            std::string t = cur.substr(0, end);
            while (!t.empty() && (t.back() == '-' || t.back() == '.')) t.pop_back();
            if (t.empty() || t == cur) break;
            v.push_back(t);
            cur = t;
        }
    }
    return v;
}

bool ParsedCommand::has_flag(const std::string& flag) const {
    return std::find(args.begin(), args.end(), flag) != args.end();
}

std::string ParsedCommand::first_positional() const {
    for (const auto& a : args) {
        if (!a.empty() && a[0] != '-') return a;
    }
    return "";
}

/* ======================================================================
 * И3.2–3.3: разбор командной строки
 *
 * Разбор рекурсивный и снимает кавычки ДО проверки. В этом и смысл
 * подхода allowlist: `cat x | sh` — это ДВЕ команды, и вторая запрещена,
 * а `echo "a | sh"` — одна команда, где `| sh` часть аргумента. Строковый
 * поиск подстроки (blocklist) этого различия не видит в принципе.
 *
 * Сознательное упрощение: содержимое скрипта не разбирается
 * ('./deploy.sh' выполняется целиком), как и команда на удалённой стороне
 * ssh. Барьер там — ключ разрешения, а не разбор (см. command_policy.h).
 * ====================================================================== */

namespace {

constexpr int kMaxParseDepth = 8;

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Метасимвол, разрывающий слово вне кавычек. '(' и ')' включены:
 * группировка — отдельная конструкция, а не часть имени файла. */
bool is_word_break(char c) {
    return is_space(c) || c == ';' || c == '&' || c == '|' || c == '<' ||
           c == '>' || c == '(' || c == ')' || c == '`';
}

/* Оператор перенаправления в позиции начала слова: >, >>, <, <<, 2>,
 * &>, &>>, >&. Возвращает длину оператора либо 0. */
size_t redirect_op_len(const std::string& s, size_t i) {
    if (i >= s.size()) return 0;
    char c = s[i];
    if (c == '&' && i + 1 < s.size() && s[i + 1] == '>') {
        return (i + 2 < s.size() && s[i + 2] == '>') ? 3u : 2u;
    }
    if (c == '>' || c == '<') {
        if (i + 1 < s.size() && s[i + 1] == c) return 2;
        /* >&file — дескриптор в файл. */
        if (c == '>' && i + 1 < s.size() && s[i + 1] == '&') return 2;
        return 1;
    }
    /* «2>», «1>»: цифра непосредственно перед оператором. */
    if (std::isdigit(static_cast<unsigned char>(c)) && i + 1 < s.size() &&
        (s[i + 1] == '>' || s[i + 1] == '<')) {
        return 2;
    }
    return 0;
}

/* Парная закрывающая скобка для «(» на позиции open, с учётом кавычек. */
size_t match_paren(const std::string& s, size_t open) {
    int depth = 0;
    char quote = 0;
    for (size_t i = open; i < s.size(); ++i) {
        char c = s[i];
        if (quote) {
            if (c == '\\' && quote == '"' && i + 1 < s.size()) ++i;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '\'' || c == '"') { quote = c; continue; }
        if (c == '\\' && i + 1 < s.size()) { ++i; continue; }
        if (c == '(') ++depth;
        else if (c == ')') {
            if (--depth == 0) return i;
        }
    }
    return std::string::npos;
}

/* Прочитать одно слово, снимая кавычки. Подстановки внутри двойных
 * кавычек для bash тоже подстановки, поэтому разбор не может их
 * пропустить: иначе политику закрывали бы одни кавычки. Найденные
 * подстановки кладутся в subs — их содержимое проверяется отдельно. */
void read_word(const std::string& s, size_t& i, std::string& out,
               std::vector<std::string>& subs) {
    while (i < s.size()) {
        char c = s[i];
        if (is_word_break(c)) break;
        /* Перенаправление посреди слова: «foo>bar» — это foo с выводом
         * в bar, а не имя «foo>bar». */
        if (redirect_op_len(s, i) > 0) break;
        if (c == '\\' && i + 1 < s.size()) {
            out += s[i + 1];
            i += 2;
            continue;
        }
        if (c == '\'') {
            ++i;
            while (i < s.size() && s[i] != '\'') {
                /* В одинарных кавычках bash ничего не раскрывает,
                 * включая обратный слэш. Идиома shell_escape('a'\''b')
                 * при этом разбирается верно: закрытие кавычки,
                 * экранированная кавычка, открытие. */
                out += s[i];
                ++i;
            }
            if (i < s.size()) ++i;
            continue;
        }
        if (c == '"') {
            ++i;
            while (i < s.size() && s[i] != '"') {
                char q = s[i];
                if (q == '\\' && i + 1 < s.size()) {
                    char n = s[i + 1];
                    /* В двойных кавычках экранируются только эти. */
                    if (n == '"' || n == '\\' || n == '$' || n == '`') {
                        out += n;
                        i += 2;
                        continue;
                    }
                    out += q;
                    ++i;
                    continue;
                }
                if (q == '$' && i + 1 < s.size() && s[i + 1] == '(') {
                    size_t end = match_paren(s, i + 1);
                    if (end == std::string::npos) { ++i; continue; }
                    subs.push_back(s.substr(i + 2, end - i - 2));
                    out += "()";
                    i = end + 1;
                    continue;
                }
                if (q == '`') {
                    size_t end = s.find('`', i + 1);
                    if (end == std::string::npos) { ++i; continue; }
                    subs.push_back(s.substr(i + 1, end - i - 1));
                    out += "()";
                    i = end + 1;
                    continue;
                }
                out += q;
                ++i;
            }
            if (i < s.size()) ++i;
            continue;
        }
        /* Подстановка вне кавычек: $( … ), ${ … }, ` … `. */
        if (c == '$' && i + 1 < s.size() && (s[i + 1] == '(' || s[i + 1] == '{')) {
            bool paren = (s[i + 1] == '(');
            size_t end = paren ? match_paren(s, i + 1) : s.find('}', i + 2);
            if (end == std::string::npos) { out += c; ++i; continue; }
            subs.push_back(s.substr(i + 2, end - i - 2));
            i = end + 1;
            continue;
        }
        if (c == '`') {
            size_t end = s.find('`', i + 1);
            if (end == std::string::npos) { out += c; ++i; continue; }
            subs.push_back(s.substr(i + 1, end - i - 1));
            i = end + 1;
            continue;
        }
        out += c;
        ++i;
    }
}

/* Слово вида FOO=bar — присваивание переменной окружения, а не программа. */
bool looks_like_assignment(const std::string& tok) {
    size_t eq = tok.find('=');
    if (eq == std::string::npos || eq == 0) return false;
    for (size_t i = 0; i < eq; ++i) {
        char c = tok[i];
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    }
    return true;
}

void parse_list(const std::string& s, std::vector<ParsedCommand>& out, int depth);

void parse_list(const std::string& s, std::vector<ParsedCommand>& out, int depth) {
    if (depth > kMaxParseDepth) {
        /* Глубже не разбираем: вложенность в 8 уровней — это уже не
         * команда, а способ спрятать её. Лучше отказать явно, чем
         * проверить внешний слой и пропустить внутренний. */
        ParsedCommand bad;
        bad.args.push_back("__depth_exceeded__");
        out.push_back(bad);
        return;
    }

    ParsedCommand cur;
    auto flush = [&]() {
        if (cur.binary.empty() && cur.args.empty() && cur.redirections.empty() &&
            cur.env.empty() && cur.sub_commands.empty()) {
            return;
        }
        out.push_back(cur);
        cur = ParsedCommand();
    };

    size_t i = 0;
    while (i < s.size()) {
        if (is_space(s[i])) { ++i; continue; }
        char c = s[i];

        if (c == ';' || c == '\n') { flush(); ++i; continue; }
        if (c == '&') {
            i += (i + 1 < s.size() && s[i + 1] == '&') ? 2 : 1;
            flush();
            continue;
        }
        if (c == '|') {
            i += (i + 1 < s.size() && s[i + 1] == '|') ? 2 : 1;
            flush();
            continue;
        }
        if (c == ')') { ++i; continue; }

        /* Группировка «( … )» — команды внутри проверяются отдельно. */
        if (c == '(') {
            size_t end = match_paren(s, i);
            if (end == std::string::npos) {
                /* Незакрытая скобка: для bash это синтаксическая ошибка,
                 * для нас — неизвестная конструкция. Отказываемся. */
                ParsedCommand bad;
                bad.args.push_back("__unclosed_paren__");
                out.push_back(bad);
                return;
            }
            std::vector<ParsedCommand> nested;
            parse_list(s.substr(i + 1, end - i - 1), nested, depth + 1);
            for (auto& n : nested) cur.sub_commands.push_back(std::move(n));
            i = end + 1;
            continue;
        }

        /* Перенаправление: «> file», «2>&1», «<<EOF». Цель — не аргумент. */
        size_t rop = redirect_op_len(s, i);
        if (rop > 0) {
            std::string op = s.substr(i, rop);
            size_t j = i + rop;
            while (j < s.size() && is_space(s[j])) ++j;
            /* «>&1» и «>&-»: дескриптор дублируется или закрывается.
             * Без этого '&' читался как оператор «&& без второй», команда
             * делилась пополам, и «1» превращался в имя программы. */
            if (j < s.size() && s[j] == '&') { op += '&'; ++j; }
            std::string target;
            std::vector<std::string> dummy;
            read_word(s, j, target, dummy);
            cur.redirections.push_back(op + target);
            i = j;
            continue;
        }

        /* Подстановка без кавычек в начале слова: ` … ` и $( … ).
         * В read_word этот случай закрыт, но там слово уже начато
         * (нужен для «echo "…`cmd`…"»). Здесь подстановка стоит сама
         * по себе, и без явной обработки она просто терялась: '&'
         * после обратной кавычки закрывал команду, а содержимое не
         * проверялось. */
        if (c == '`' || (c == '$' && i + 1 < s.size() &&
                         (s[i + 1] == '(' || s[i + 1] == '{'))) {
            bool paren = (c == '$');
            char open = paren ? s[i + 1] : '`';
            size_t end = (!paren) ? s.find('`', i + 1)
                                  : (open == '(' ? match_paren(s, i + 1)
                                                 : s.find('}', i + 2));
            if (end == std::string::npos) {
                ++i;
                continue;
            }
            std::vector<ParsedCommand> nested;
            parse_list(s.substr(i + 2, end - i - 2), nested, depth + 1);
            cur.has_substitution = true;
            for (auto& n : nested) cur.sub_commands.push_back(std::move(n));
            i = end + 1;
            continue;
        }

        std::string word;
        std::vector<std::string> subs;
        size_t before = i;
        read_word(s, i, word, subs);
        if (i == before) { ++i; continue; }

        if (!subs.empty()) {
            cur.has_substitution = true;
            for (const auto& sub : subs) {
                std::vector<ParsedCommand> nested;
                parse_list(sub, nested, depth + 1);
                for (auto& n : nested) cur.sub_commands.push_back(std::move(n));
            }
        }
        if (word.empty()) continue;

        if (cur.binary.empty() && cur.args.empty() && looks_like_assignment(word)) {
            cur.env.push_back(word);
        } else if (cur.binary.empty()) {
            cur.binary = word;
        } else {
            cur.args.push_back(word);
        }
    }
    flush();
}

/* Таблицы политики — std::vector, а не массивы указателей (найдено на
 * границе И5, чинится отдельным коммитом поверх граничного).
 *
 * Массив `const char* kX[] = {...}` без явного nullptr читался до первого
 * нулевого указателя, то есть ЗА пределами своего объявления: сколько
 * «записей» в списке разрешённых программ, решал линкер, раскладкой
 * .rodata. В сборке с -O2 сразу за таблицей разрешённых лежала таблица
 * ЗАПРЕЩЁННЫХ, и в allowlist попадали bash, docker, cron, ssh, deploy —
 * то есть «закрытый список» переставал быть закрытым, и отказ
 * /srv/app/deploy.sh превращался в разрешение. В сборке без оптимизации
 * следом случайно оказывался ноль, и всё выглядело исправным: тот же
 * класс, что у бинарника месячной давности (D22), только у данных.
 *
 * Теперь размер таблицы задаёт компилятор: «забытый терминатор»
 * невозможен не по соглашению, а по типам. */
bool in_list(const std::vector<const char*>& list, const std::string& name) {
    for (const char* item : list) {
        if (item && name == item) return true;
    }
    return false;
}

/* --- Таблицы политики --- */

/* Что можно запускать. Список намеренно щедрый: это инструмент для
 * работы с чужим кодом, и «нельзя запустить npm» делает агента
 * бесполезным. Задача allowlist не в том, чтобы сузить круг полезных
 * команд, а в том, чтобы в нём НЕ оказались: программы, стирающие
 * данные, и универсальные обходы (вложенная оболочка, xargs, env) —
 * иначе политику обходят одной строкой.
 *
 * Второй слой (PermissionEngine) по умолчанию спрашивает пользователя на
 * каждую команду bash, поэтому широкий allowlist означает много вопросов,
 * а не много вреда. */
const std::vector<const char*> kAllowedBinaries = {
    /* Файлы и текст. */
    "cat", "ls", "less", "more", "head", "tail", "wc", "grep", "egrep", "fgrep",
    "rg", "ag", "ack", "find", "fd", "tree", "file", "stat", "du", "df", "diff",
    "cmp", "sort", "uniq", "cut", "tr", "sed", "awk", "gawk", "jq", "yq", "xxd",
    "od", "base64", "md5sum", "sha1sum", "sha256sum", "realpath", "readlink",
    "basename", "dirname", "pwd", "echo", "printf", "true", "false", "test",
    "expr", "date", "seq", "nl", "strings", "split", "tac", "paste", "comm",
    "column", "fold", "join", "rev", "shuf", "which", "type", "man", "info",
    "tee", "touch", "mkdir", "cp", "mv", "ln", "rm", "chmod", "chown", "chgrp",
    "mktemp", "truncate", "tar", "gzip", "gunzip", "bzip2", "bunzip2", "xz",
    "zip", "unzip", "rsync", "scp",
    /* Процессы и система — для диагностики. */
    "ps", "kill", "killall", "uptime", "id", "whoami", "hostname", "uname",
    "printenv", "ldd", "lsof", "free", "vmstat", "iostat", "lscpu", "lsblk",
    "systemctl", "service", "journalctl", "crontab", "nice", "ionice",
    "taskset", "timeout", "stdbuf",
    /* Сеть. */
    "curl", "wget", "ping", "dig", "host", "nslookup", "nc", "ssh", "sftp",
    /* Сборка и разработка. */
    "make", "cmake", "ninja", "gcc", "g++", "clang", "clang++", "cc", "c++",
    "ar", "nm", "objdump", "pkg-config", "strip",
    "python", "pip", "pip3", "pipx", "pytest", "tox", "node", "npm", "npx",
    "yarn", "pnpm", "tsc", "eslint", "php", "composer", "phpunit", "wp",
    "mysql", "mysqldump", "mariadb", "mariadb-dump", "psql", "pg_dump",
    "sqlite3", "redis-cli", "git", "svn", "hg", "diff-so-fancy", "sudo",
    "apachectl", "apache2ctl", "apache2", "httpd", "nginx", "docker",
    "docker-compose", "podman", "kubectl", "helm", "minikube", "apt",
    "apt-cache", "apt-get", "dpkg", "dnf", "yum", "snap", "rustc", "cargo", "go",
    "gofmt", "javac", "java", "dotnet", "mvn", "gradle", "bundle",
    "shellcheck", "shfmt", "hadolint",
};

/* Запрещены всегда — и в произвольной команде, и в собранной плагином.
 * Первая группа: необратимое разрушение носителей. Вторая: обход
 * политики. «Запустить вложенную оболочку», «передать строку
 * интерпретатору», «выполнить то, что подставит переменная» — всё это
 * не разбирается построчно, поэтому запрещено целиком. Список
 * собранных плагином команд (deploy.sh, sudo mariadb, php8.1 -l,
 * '/usr/local/bin/composer') сюда не попадает: имя программы там
 * выбрал код, а allowlist к таким командам не применяется. */
const std::vector<const char*> kForbiddenBinaries = {
    "dd", "mkfs", "fdisk", "sfdisk", "cfdisk", "shred", "badblocks", "wipefs",
    "hdparm", "partprobe", "mkswap",
    "sh", "bash", "zsh", "dash", "ksh", "csh", "tcsh", "ash", "busybox",
    "env", "xargs", "nohup", "setsid", "at", "batch", "screen", "tmux",
    "script", "su", "doas", "eval", "exec", "command", "builtin", "watch",
    "parallel", "strace", "ltrace", "gdb", "chroot", "unshare", "nsenter",
};

/* Переменные окружения, меняющие смысл запуска. execve разворачивает их
 * ДО того, как что-либо проверили, поэтому подмена PATH или
 * LD_PRELOAD обходит любую проверку команды. */
const std::vector<const char*> kForbiddenEnvVars = {
    "PATH", "LD_PRELOAD", "LD_LIBRARY_PATH", "LD_AUDIT", "IFS", "BASH_ENV",
    "ENV", "SHELL", "PYTHONPATH", "PYTHONSTARTUP", "PERL5OPT", "PERL5LIB",
    "RUBYOPT", "NODE_OPTIONS", "GIT_SSH", "GIT_SSH_COMMAND", "GIT_EXTERNAL_DIFF",
    "GIT_PAGER", "PAGER", "EDITOR", "VISUAL", "GIT_ALLOW_PROTOCOL", "GIT_CONFIG",
    "GIT_DIR", "GIT_WORK_TREE", "PROMPT_COMMAND",
};

/* Флаги, принимающие значение отдельным аргументом. Нужны, чтобы
 * «первый не-флаг» оказался подкомандой, а не её значением:
 * `git -C /path status` → status, а не /path. */
const std::vector<const char*> kValuedFlags = {
    "-C", "-c", "-u", "-p", "-f", "-o", "-m", "-d", "-e", "-n", "-w", "-i",
    "-r", "-s", "-a", "-t", "-g", "-U",
    "--path", "--user", "--file", "--output", "--message", "--config",
    "--context", "--host", "--port", "--name", "--dbname", "--dbuser",
    "--dbpass", "--workdir", "--entrypoint", "--env", "--label", "--volume",
    "--mount", "--network", "--format", "--target", "--source", "--destination",
    "--user-data", "--chdir", "--cwd", "--exclude", "--format",
};

bool flag_takes_value(const std::string& f) {
    return in_list(kValuedFlags, f);
}

/* Значение, которое политика не может разрешить в путь: переменная,
 * подстановка, маска, домашний каталог. Для записи (перенаправление,
 * curl --output) это всегда запрет. */
bool is_unknown_path(const std::string& p) {
    if (p.empty()) return false;
    return p.find('$') != std::string::npos || p.find('`') != std::string::npos ||
           p.find('*') != std::string::npos || p.find('?') != std::string::npos ||
           p.find('~') != std::string::npos || p.find('{') != std::string::npos;
}

/* Куда нельзя писать: системные каталоги, блочные устройства, sudoers.
 *
 * /dev/null и подобные символьные устройства РАЗРЕШЕНЫ: «2>/dev/null» и
 * «curl -o /dev/null» встречаются в каждой второй собираемой команде
 * (systemctl, curl --version | head -1, health check сайта). Запрещать
 * их — значит сломать плагин, а не защитить его; опасны блочные
 * устройства (/dev/sda, /dev/nvme0n1), а не /dev/null. */
bool is_dangerous_target(const std::string& p) {
    if (p.empty()) return false;
    if (is_unknown_path(p)) return true;

    static const std::vector<const char*> kSafeDevices = {
        "/dev/null", "/dev/zero", "/dev/random", "/dev/urandom", "/dev/tty",
        "/dev/stdout", "/dev/stderr", "/dev/stdin", "/dev/fd/",
    };
    static const size_t ns = kSafeDevices.size();
    for (size_t i = 0; i < ns; ++i) {
        std::string pre = kSafeDevices[i];
        if (p == pre) return false;
        if (pre.back() == '/' && p.compare(0, pre.size(), pre) == 0) return false;
    }

    /* Блочные устройства: /dev/sda, /dev/nvme0n1, /dev/hda, /dev/vda. */
    static const std::vector<const char*> kDevices = {"/dev/sd", "/dev/nvme", "/dev/hd", "/dev/vd"};
    static const size_t nd = kDevices.size();
    for (size_t i = 0; i < nd; ++i) {
        if (p.compare(0, std::string(kDevices[i]).size(), kDevices[i]) == 0)
            return true;
    }

    static const std::vector<const char*> kPrefixes = {
        "/etc", "/boot", "/proc", "/sys", "/root/.ssh", "/usr/bin", "/usr/sbin",
        "/usr/lib", "/bin", "/sbin", "/lib", "/lib64", "/var/run/sudo",
    };
    static const size_t n = kPrefixes.size();
    for (size_t i = 0; i < n; ++i) {
        std::string pre = kPrefixes[i];
        if (p == pre) return true;
        if (p.size() > pre.size() && p.compare(0, pre.size(), pre) == 0 &&
            p[pre.size()] == '/') {
            return true;
        }
    }
    return p.find("sudoers") != std::string::npos;
}

/* Рекурсивный флаг: -r, -rf, -Rf, --recursive. */
bool is_recursive_flag(const std::string& a) {
    if (a.size() < 2 || a[0] != '-') return false;
    if (a == "--recursive" || a == "-R") return true;
    if (a.compare(0, 2, "--") == 0) return false;
    return a.find('r') != std::string::npos || a.find('R') != std::string::npos;
}

/* «-c», «-rf», «--exec-path»: флаг ровно такой или с присоединённым
 * значением. Длинные флаги через «=» не разбираем — они идут своим
 * списком. */
bool is_flag_or_attached(const std::string& a, const std::string& flag) {
    if (a == flag) return true;
    if (a.size() <= flag.size()) return false;
    if (a.compare(0, flag.size(), flag) != 0) return false;
    /* Присоединённое значение только для коротких флагов: -cFOO это
     * -c с FOO, а --foo=bar — отдельная форма, её ловит == и compare. */
    return flag.size() == 2 && flag[1] != '-';
}

}  // namespace

std::vector<ParsedCommand> parse_command_line(const std::string& cmd) {
    std::vector<ParsedCommand> out;
    parse_list(cmd, out, 0);
    return out;
}

/* ======================================================================
 * CommandPolicy
 * ====================================================================== */

CommandPolicy& command_policy() {
    /* Одна политика на процесс. Создание — thread-safe (стандарт C++11
     * для function-local static), правка хостов — под мьютексом внутри
     * класса. Политика не хранит состояния сессии и ничего не знает про
     * Engine, поэтому юнит-тесты строят свои экземпляры на стеке. */
    static CommandPolicy policy;
    return policy;
}

CommandPolicy::CommandPolicy() {
    for (const char* binary : kAllowedBinaries) allowed_.insert(binary);
    /* Базовое правило — Ask, а не Allow. Смысл: «решает allowlist».
     * Если бы база была Allow, правило сработало бы раньше проверки
     * списка и allowlist перестал бы существовать. Явный Allow от
     * пользователя — единственное, что открывает бинарник сверх списка;
     * Deny — единственное, что закрывает (правило не отменяет
     * кодовые запреты, см. check_one). */
    arg_rules_.push_back(Rule{"*", "*", PermissionAction::Ask, ""});
    /* Локальная машина доверена по умолчанию: агент работает с
     * localhost-окружением (health check сайта, локальные API), и без
     * этого каждая проверка curl к своему же сайту спрашивала бы
     * разрешение. Внешние хосты открываются настройкой — Engine
     * подкладывает wp_site_url и wp_local_url (И3.6). */
    trusted_hosts_.insert("localhost");
    trusted_hosts_.insert("127.0.0.1");
    trusted_hosts_.insert("::1");
    trusted_hosts_.insert("0.0.0.0");
}

/* --- Настройка --- */

bool CommandPolicy::binary_allowed(const std::string& binary) const {
    ParsedCommand c;
    c.binary = binary;
    for (const auto& v : c.variants()) {
        if (allowed_.count(v) > 0) return true;
    }
    return false;
}

void CommandPolicy::allow_binary(const std::string& binary) {
    allowed_.insert(binary);
}

void CommandPolicy::deny_binary(const std::string& binary) {
    allowed_.erase(binary);
}

void CommandPolicy::add_arg_rule(Rule r) {
    arg_rules_.push_back(std::move(r));
}

void CommandPolicy::add_arg_rule(const std::string& permission,
                                 const std::string& pattern,
                                 PermissionAction action) {
    arg_rules_.push_back(Rule{permission, pattern, action, ""});
}

void CommandPolicy::clear_arg_rules() {
    arg_rules_.clear();
}

/* --- Сеть --- */

void CommandPolicy::trust_host(const std::string& host) {
    if (host.empty()) return;
    std::lock_guard<std::mutex> lk(host_mtx_);
    trusted_hosts_.insert(host);
}

void CommandPolicy::clear_trusted_hosts() {
    std::lock_guard<std::mutex> lk(host_mtx_);
    trusted_hosts_.clear();
}

bool CommandPolicy::host_trusted(const std::string& host) const {
    if (host.empty()) return false;
    std::lock_guard<std::mutex> lk(host_mtx_);
    return trusted_hosts_.count(host) > 0;
}

std::set<std::string> CommandPolicy::trusted_hosts() const {
    std::lock_guard<std::mutex> lk(host_mtx_);
    return trusted_hosts_;
}

std::string CommandPolicy::host_from_url(const std::string& arg) {
    size_t p = arg.find("://");
    if (p == std::string::npos) return "";
    size_t start = p + 3;
    /* Учётные данные в URL не влияют на имя хоста. */
    size_t slash = arg.find('/', start);
    size_t at = arg.find('@', start);
    if (at != std::string::npos && (slash == std::string::npos || at < slash))
        start = at + 1;
    size_t end = arg.find('/', start);
    std::string hostport = (end == std::string::npos)
                               ? arg.substr(start)
                               : arg.substr(start, end - start);
    /* IPv6 в квадратных скобках: [::1]:8080. Проверяем ДО отбрасывания
     * порта — иначе первый ':' внутри скобок срезал бы адрес до «[». */
    if (hostport.size() >= 2 && hostport.front() == '[') {
        size_t rb = hostport.find(']');
        if (rb != std::string::npos) return hostport.substr(1, rb - 1);
    }
    size_t colon = hostport.find(':');
    if (colon != std::string::npos) hostport = hostport.substr(0, colon);
    return hostport;
}

/* --- Валидаторы (И3.4) --- */

std::string CommandPolicy::check_env(const ParsedCommand& c) const {
    for (const auto& e : c.env) {
        size_t eq = e.find('=');
        std::string name = (eq == std::string::npos) ? e : e.substr(0, eq);
        if (in_list(kForbiddenEnvVars, name)) {
            return "установка переменной окружения " + name +
                   " перед командой запрещена: она меняет смысл запуска"
                   " (путь поиска, пути библиотек, поведение"
                   " интерпретатора) и обходит проверку команды";
        }
    }
    return "";
}

std::string CommandPolicy::check_git(const ParsedCommand& c) const {
    for (const auto& a : c.args) {
        /* -c — переопределение конфигурации на лету. Ровно этим флагом
         * включают посторонний исполнитель: `git -c core.pager='!sh' log`
         * и `git -c core.sshCommand=... clone`. Запрещён флаг целиком:
         * опасные ключи не перечислить, их десятки и их добавляют. */
        if (is_flag_or_attached(a, "-c") || a == "--config-env" ||
            a.compare(0, 13, "--config-env=") == 0) {
            return "git с ключом -c/--config-env запрещён: переопределение"
                   " конфигурации на лету включает посторонние команды"
                   " (core.pager, core.sshCommand, alias) и обходит"
                   " политику команд";
        }
        if (a == "--exec-path" || a.compare(0, 12, "--exec-path=") == 0) {
            return "git --exec-path запрещён: подменяет исполняемые файлы"
                   " git своими";
        }
        /* «!» в аргументе — это shell-команда: так заводят alias
         * (`git config alias.deploy '!rm -rf ~'`) и pager. */
        if (!a.empty() && a[0] == '!') {
            return "git с аргументом, начинающимся с '!', запрещён: '!'"
                   " в конфигурации git означает выполнение shell-команды";
        }
        if (a.find("=!") != std::string::npos) {
            return "git со значением вида '=!' запрещён: это shell-команда"
                   " в конфигурации git (alias, pager, external diff)";
        }
    }

    /* `git config core.editor …` — закладка на будущее: исполнять его
     * будет следующий git commit, то есть запрет на -c и alias тут
     * обходится персистентной настройкой. Ключи перечислены, потому что
     * список из трёх-четырёх неполон ровно настолько же, насколько
     * неполон был исходный blocklist из девяти подстрок. */
    if (c.first_positional() == "config") {
        static const std::vector<const char*> kHookKeys = {
            "pager", "editor", "sshcommand", "alias", "hookspath", "askpass",
            "credential.helper", "diff.external", "difftool", "mergetool",
            "fsmonitor", "filter", "uploadpack", "receivepack", "core.pager",
            "core.editor", "sequence.editor", "protocol.allow",
        };
        static const size_t nh = kHookKeys.size();
        for (const auto& a : c.args) {
            std::string lower = a;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            for (size_t k = 0; k < nh; ++k) {
                if (lower.find(kHookKeys[k]) == std::string::npos) continue;
                return "git config с ключом " + a + " запрещён: это"
                       " персистентный вызов посторонней программы (pager,"
                       " editor, alias, hook). Он сработает при СЛЕДУЮЩЕЙ"
                       " команде git, минуя проверку этой";
            }
        }
    }
    return "";
}

std::string CommandPolicy::check_sudo(const ParsedCommand& c) const {
    for (const auto& a : c.args) {
        /* -E сохраняет окружение вызывающего: вместе с PATH и
         * LD_PRELOAD это даёт выполнение кода от имени root. */
        if (a == "-E" || a == "--preserve-env" ||
            a.compare(0, 15, "--preserve-env=") == 0) {
            return "sudo -E/--preserve-env запрещён: сохранение окружения"
                   " вместе с PATH и LD_PRELOAD даёт выполнение кода"
                   " от имени root";
        }
        /* -S читает пароль из stdin: отдаёт его тому, кто пишет в поток. */
        if (a == "-S" || a == "--stdin") {
            return "sudo -S/--stdin запрещён: пароль читается из потока,"
                   " то есть отдаётся тому, кто туда пишет";
        }
        if (a.find("sudoers") != std::string::npos) {
            return "запись в sudoers запрещена";
        }
    }
    const std::string verb = c.first_positional();
    if (verb == "visudo" || verb == "tee") {
        return "sudo " + verb + " запрещён: изменение правил sudo даёт"
               " root без пароля";
    }
    return "";
}

std::string CommandPolicy::check_network(const ParsedCommand& c) const {
    const std::string prog = c.program();

    /* Флаги, превращающие «скачать» в «выгрузить» или «съесть файл». */
    static const std::vector<const char*> kUploadFlags = {
        "-K", "--config", "-T", "--upload-file", "--upload-files", "-d",
        "--data", "--data-raw", "--data-binary", "--data-urlencode", "-F",
        "--form", "--form-string", "--post-file", "--post-data",
    };
    for (size_t i = 0; i < c.args.size(); ++i) {
        const std::string& a = c.args[i];
        if (in_list(kUploadFlags, a)) {
            return prog + " " + a + " запрещён: это выгрузка данных на"
                   " сторонний хост или чтение произвольного файла"
                   " конфигурации, а не скачивание";
        }
        /* Куда пишем: -o FILE, --output FILE, --output-document=FILE,
         * -oFILE. */
        std::string target;
        bool has_target = false;
        if (a == "-o" || a == "--output" || a == "--output-document") {
            if (i + 1 < c.args.size()) {
                target = c.args[i + 1];
                has_target = true;
            }
        } else if (is_flag_or_attached(a, "-o") || is_flag_or_attached(a, "-O")) {
            target = a.substr(2);
            has_target = true;
        } else if (a.compare(0, 9, "--output=") == 0) {
            target = a.substr(9);
            has_target = true;
        }
        if (has_target && is_dangerous_target(target)) {
            return prog + ": запись в " + target + " запрещена (системный"
                   " каталог, устройство или значение, которое политика"
                   " не может разрешить в путь)";
        }
    }
    return "";
}

std::string CommandPolicy::check_remove(const ParsedCommand& c) const {
    bool recursive = false;
    std::vector<std::string> targets;
    for (const auto& a : c.args) {
        if (!a.empty() && a[0] == '-') {
            if (is_recursive_flag(a)) recursive = true;
            continue;
        }
        targets.push_back(a);
    }
    if (!recursive) return "";

    for (const auto& t : targets) {
        if (t.empty()) continue;
        /* Рекурсивное удаление — единственное, где цель важна целиком.
         * Абсолютный путь, домашний каталог и значение, которое
         * невозможно разрешить ($, `), обязаны падать: `rm -rf ~` и
         * `rm -rf $HOME` — это не то, что пользователь собирался
         * удалять, даже если он так написал.
         *
         * Маски (`*.log`) пропускаем: они раскрываются в текущем
         * каталоге, а команда всё равно спрашивает разрешения у
         * пользователя (ключ bash, дефолт — Ask). Запрет всех масок
         * обошёл бы обычную уборку и приучил бы к обходу политики. */
        if (t[0] == '/' || is_unknown_path(t)) {
            return "rm -r " + t + " запрещён: рекурсивное удаление"
                   " абсолютного пути, домашнего каталога или значения,"
                   " которое нельзя разрешить в путь. Удали относительно"
                   " каталога: cd <каталог> && rm -r <имя>";
        }
        if (t == ".." || t.compare(0, 3, "../") == 0 ||
            t.find("/../") != std::string::npos) {
            return "rm -r " + t + " запрещён: цель выходит выше"
                   " текущего каталога";
        }
    }
    return "";
}

std::string CommandPolicy::check_interpreter(const ParsedCommand& c) const {
    const std::vector<std::string> names = c.variants();
    /* «Код из строки» минует любой разбор аргументов: политика видит
     * python3 и текст программы, но не видит, что внутри. Запуск файла
     * (python3 script.py) разрешён — запрещена передача программы
     * аргументом. */
    static const struct { const char* prog; const char* flag; } kCodeFlags[] = {
        {"python", "-c"}, {"python3", "-c"}, {"perl", "-e"}, {"perl", "-E"},
        {"ruby", "-e"}, {"node", "-e"}, {"node", "--eval"}, {"node", "-p"},
        {"node", "--print"}, {"php", "-r"}, {"php", "-R"}, {"lua", "-e"},
    };
    for (const auto& e : kCodeFlags) {
        if (std::find(names.begin(), names.end(), e.prog) == names.end()) continue;
        for (const auto& a : c.args) {
            if (is_flag_or_attached(a, e.flag)) {
                return c.program() + " " + e.flag + " запрещён: выполнение кода"
                       " из аргумента не разбирается политикой. Запишите"
                       " программу в файл проекта и запустите файл";
            }
        }
    }
    return "";
}

std::string CommandPolicy::check_awk(const ParsedCommand& c) const {
    static const std::vector<const char*> kAawks = {"awk", "gawk", "mawk", "nawk"};
    const std::vector<std::string> names = c.variants();
    bool is_awk = false;
    for (const auto& v : names) {
        if (in_list(kAawks, v)) { is_awk = true; break; }
    }
    if (!is_awk) return "";
    /* У awk нет флага с программой — она всегда positional, поэтому
     * system() не спрятан за ключом и проверяется здесь. */
    for (const auto& a : c.args) {
        if (a.find("system(") != std::string::npos ||
            a.find("getline") != std::string::npos) {
            return "awk с system()/getline запрещён: это запуск"
                   " произвольной shell-команды из программы awk";
        }
    }
    return "";
}

/* --- Проверка --- */

std::string CommandPolicy::check(const std::string& cmd) const {
    /* Пустая строка — отказ на входе, а не в check_list: у команды
     * без вложенных подкоманд список подкоманд пуст, и проверка «пусто
     * значит плохо» там отклоняла бы вообще всё. */
    if (cmd.find_first_not_of(" \t\n\r;|&#") == std::string::npos)
        return "пустая команда: выполнять нечего";
    return check_list(parse_command_line(cmd), true);
}

std::string CommandPolicy::check_assembled(const std::string& cmd) const {
    return check_list(parse_command_line(cmd), false);
}

std::string CommandPolicy::check_list(const std::vector<ParsedCommand>& cmds,
                                      bool strict) const {
    for (const auto& c : cmds) {
        std::string r = check_one(c, strict);
        if (!r.empty()) return r;
    }
    return "";
}

std::string CommandPolicy::check_one(const ParsedCommand& c, bool strict) const {
    /* Маркеры отказа парсера: незакрытая скобка, вложенность не
     * разобрана. Оба означают одно — «политика не знает, что здесь
     * будет выполнено», и обязаны приводить к отказу, а не к пропуску. */
    for (const auto& a : c.args) {
        if (a == "__unclosed_paren__") {
            return "команда не разобрана (незакрытая скобка): политика"
                   " не знает, что будет выполнено";
        }
        if (a == "__depth_exceeded__") {
            return "команда не разобрана (вложенность больше 8 уровней):"
                   " политика не знает, что будет выполнено";
        }
    }

    std::string err = check_env(c);
    if (!err.empty()) return err;

    for (const auto& r : c.redirections) {
        std::string target = r;
        size_t gt = target.find('>');
        size_t lt = target.find('<');
        size_t cut = (gt == std::string::npos) ? lt : gt;
        if (cut != std::string::npos && cut + 1 < target.size())
            target = target.substr(cut + 1);
        /* «2>&1», «>&-» — не запись в файл. */
        if (target.empty() || target == "&1" || target == "&2" ||
            target == "-" || target == "&-")
            continue;
        if (is_dangerous_target(target)) {
            return "перенаправление в " + target + " запрещено (системный"
                   " каталог, устройство или значение, которое политика"
                   " не может разрешить в путь)";
        }
    }

    if (c.binary.empty()) {
        /* Стадия, целиком состоящая из подстановки: `ls | $(which sh)`
         * в bash выполняет ПРЯМОЙ ВЫВОД другой команды. Политика вывод
         * не разбирает, поэтому такая стадия — отказ в обоих режимах. */
        if (c.has_substitution && c.args.empty() && c.redirections.empty() &&
            c.env.empty()) {
            return "команда подставляется из вывода другой команды"
                   " (конвейер или $( … ) вместо программы): политика не"
                   " знает, что будет выполнено. Назовите программу явно";
        }
        /* Группировка «( a; b )» — у неё нет своей программы. */
        if (c.args.empty() && c.redirections.empty() && c.env.empty())
            return check_list(c.sub_commands, strict);
        return "команда не разобрана: не удалось определить программу."
               " Политика не знает, что будет выполнено";
    }

    const std::string prog = c.program();
    if (prog.empty()) {
        return "команда не разобрана: пустое имя программы";
    }

    /* --- Уровень 1: кодовые запреты. Правило их НЕ отменяет. --- */
    bool forbidden = false;
    for (const auto& v : c.variants()) {
        if (in_list(kForbiddenBinaries, v)) {
            forbidden = true;
            break;
        }
    }
    if (forbidden) {
        return prog + " запрещён политикой команд: необратимое"
               " разрушение носителей и универсальные обходы (вложенная"
               " оболочка, xargs, env) не разбираются построчно, поэтому"
               " запрещены целиком";
    }

    /* --- Уровень 2: правила политики (слой пользователя), last match
     * wins — как в Ruleset (И2.2). --- */
    PermissionAction action = PermissionAction::Ask;
    {
        const std::string text = c.text();
        for (const auto& r : arg_rules_) {
            if (r.permission != "*" && r.permission != prog) continue;
            if (!Wildcard::match(r.pattern, text)) continue;
            action = r.action;
        }
    }
    if (action == PermissionAction::Deny) {
        return prog + " запрещён правилом политики команд (" + c.text() + ")";
    }

    /* --- Уровень 3: allowlist бинарников.
     * Только для произвольных команд: в собранных плагином программу
     * выбрал код (deploy.sh, sudo mariadb, /usr/bin/php8.1), и список
     * там был бы не защитой, а источником ложных отказов. --- */
    if (action != PermissionAction::Allow && strict && !binary_allowed(prog)) {
        return prog + " нет в списке разрешённых программ. Список"
               " закрытый: в нём команды работы с кодом и файлами, но не"
               " программы необратимого действия и не обходы. Для"
               " сетевых запросов есть web_fetch, для работы с сервером —"
               " инструменты модуля devops";
    }

    /* --- Уровень 4: валидаторы по бинарнику.
     * Сопоставление по вариантам имени: «php8.1» обязан получить те же
     * проверки, что и «php», иначе версия дистрибутива отключала бы
     * запрет на -r одной переустановкой пакета. --- */
    auto is_prog = [&c](const char* name) {
        for (const auto& v : c.variants()) {
            if (v == name) return true;
        }
        return false;
    };
    if (is_prog("git")) {
        err = check_git(c);
        if (!err.empty()) return err;
    }
    if (is_prog("sudo") || is_prog("doas")) {
        err = check_sudo(c);
        if (!err.empty()) return err;
    }
    if (is_prog("curl") || is_prog("wget") || is_prog("wget2")) {
        err = check_network(c);
        if (!err.empty()) return err;
        if (strict) {
            std::string host;
            for (const auto& a : c.args) {
                if (!a.empty() && a[0] == '-') continue;
                host = host_from_url(a);
                if (!host.empty()) break;
            }
            if (!host.empty() && !host_trusted(host)) {
                return prog + " к хосту " + host + " запрещён: в bash"
                       " сеть ограничена доверенными хостами (по умолчанию"
                       " только localhost). Для запроса к произвольному"
                       " адресу есть web_fetch";
            }
        }
    }
    if (is_prog("rm")) {
        err = check_remove(c);
        if (!err.empty()) return err;
    }
    err = check_interpreter(c);
    if (!err.empty()) return err;
    err = check_awk(c);
    if (!err.empty()) return err;
    if (is_prog("find")) {
        for (const auto& a : c.args) {
            if (a == "-exec" || a == "-execdir" || a == "-ok" || a == "-okdir") {
                return "find " + a + " запрещён: запуск найденных файлов"
                       " не разбирается политикой. Ограничьте поиск"
                       " каталогом проекта и обработайте результат"
                       " отдельным вызовом";
            }
        }
    }
    /* chmod/chown -R по корню — из старого blocklist, сохранённого как
     * проверка: «chmod -R 777 /» в allowlist попадает. */
    if (is_prog("chmod") || is_prog("chown") || is_prog("chgrp")) {
        bool recursive = false;
        for (const auto& a : c.args) {
            if (is_recursive_flag(a)) recursive = true;
        }
        if (recursive) {
            for (const auto& a : c.args) {
                if (!a.empty() && a[0] == '-') continue;
                if (a == "/" || a == "/*" || a == "~" ||
                    a.compare(0, 2, "/e") == 0) {
                    return prog + " -R " + a + " запрещён: рекурсивная смена"
                           " прав по всей файловой системе";
                }
            }
        }
    }
    /* Подстановка в команде, которую собрал плагин, — всегда ошибка
     * сборки: аргумент не прошёл экранирование. В произвольной команде
     * подстановка допустима, но её содержимое проверяется рекурсивно
     * (sub_commands). */
    if (!strict && c.has_substitution) {
        return prog + ": в команде, которую собирает плагин, найдена"
               " подстановка — это признак неэкранированного аргумента"
               " (как в wp_create_site до И3: кавычки SQL попадали в"
               " двойные кавычки shell и выполнялись как команда). Такой"
               " вызов запрещён, исправьте сборку команды";
    }

    return check_list(c.sub_commands, strict);
}

/* ======================================================================
 * И3.5: паттерн для кнопки «всегда»
 * ====================================================================== */

std::string CommandPolicy::always_pattern(const std::string& cmd) const {
    std::vector<ParsedCommand> cmds = parse_command_line(cmd);
    /* Несколько команд или вложенная подстановка: безопасного префикса
     * не существует, а выдуманный префикс разрешил бы команду, которой
     * пользователь не видел. «*» означает «спросить в следующий раз». */
    if (cmds.size() != 1 || !cmds[0].sub_commands.empty()) return "*";

    const ParsedCommand& c = cmds[0];
    if (c.binary.empty()) return "*";

    /* Префикс — имя в том виде, в каком оно написано: шаблон будет
     * сопоставляться с текстом команды, который начинается именно с
     * него. «php *» не совпало бы с «php8.1 -l index.php», то есть
     * кнопка «всегда» не работала бы для версионированных имён. */
    std::string pattern = c.program();

    /* Подкоманда = первый аргумент, который не флаг и не значение флага.
     * «git -C /path status» → status, а не /path: иначе «всегда разрешить
     * git status» превратилось бы в «git -C *», то есть в любой git на
     * любом каталоге — ровно тот класс ошибки, который И3.5 и закрывает. */
    bool skip_next = false;
    for (const auto& a : c.args) {
        if (skip_next) { skip_next = false; continue; }
        if (a.empty()) continue;
        if (a[0] == '-') {
            if (a.compare(0, 2, "--") == 0) {
                /* Длинный флаг со значением: у «--path=/x» значение уже
                 * слито, у «--path /x» — отдельным аргументом. */
                if (a.find('=') == std::string::npos) skip_next = true;
            } else if (flag_takes_value(a)) {
                skip_next = true;
            }
            continue;
        }
        pattern += " " + a;
        break;
    }
    return pattern + " *";
}

}  // namespace coder
