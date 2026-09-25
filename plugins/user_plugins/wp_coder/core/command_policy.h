#pragma once

/*
 * command_policy.h — Политика команд: blocklist → allowlist (И3).
 *
 * Дефект D5: у плагина был blocklist из девяти подстрок
 * (core/security.cpp, is_command_allowed). Он ловил ровно то, что в него
 * вписали, поэтому `rm -rf ~` (нет подстроки "rm -rf /") проходил, а
 * `rm -fr /` — нет, потому что substring ищет "rm -rf /" буквально.
 * Перечислять опасные команды бесконечно нельзя: список всегда длиннее
 * списка запрещённого. Поэтому здесь обратная логика — бинарник НЕ
 * входит в allowlist, значит не выполняется.
 *
 * ПОЧЕМУ ЭТО НЕ ДУБЛЬ СИСТЕМЫ РАЗРЕШЕНИЙ (И2). Разрешения отвечают на
 * вопрос «можно ли это пользователю» — про то, стоит ли спросить. Политика
 * команд отвечает на другой вопрос: «безопасно ли это вообще». Правило
 * «bash * → allow» не должно превращать `rm -rf /` в разрешённое, иначе
 * allowlist превратился бы в украшение. Поэтому:
 *
 *   - PermissionEngine (И2) живёт в ToolRunner::run и спрашивает.
 *   - CommandPolicy живёт в shell-обёртке и не спрашивает никого: он
 *     возвращает отказ, который пользователь подтвердить не может.
 *
 * ПОЧЕМУ ДВА РЕЖИМА ПРОВЕРКИ. check() — полная, с allowlist бинарников:
 * её вызывает exec_command, где бинарник выбирает модель, и именно там
 * список «можно / нельзя» что-то значит. check_assembled() — только
 * запреты (dd, mkfs, sudo -E, git -c core.pager=, curl --upload-file,
 * подстановки) без allowlist: её вызывает обёртка shell::run_capture
 * для 49 остальных инструментов, где бинарник выбрал КОД плагина, а не
 * модель. Для них allowlist был бы не защитой, а источником ложных
 * отказов: deploy выполняет './deploy.sh', wp_create_site зовёт sudo
 * mariadb, php_lint запускает '/usr/bin/php8.1'. Запреты при этом
 * работают одинаково: если когда-нибудь сборка команды от аргумента
 * модели даст `dd of=/dev/sda`, откажет и она.
 *
 * ЧЕГО ПОЛИТИКА НЕ ДЕЛАЕТ (важно для честности):
 *   - не разбирает содержимое скрипта: './deploy.sh' выполняется целиком,
 *     и команды внутри файла не видны. Барьер здесь — ключ разрешения
 *     «deploy» и требование, чтобы скрипт лежал в проекте;
 *   - не разбирает удалённую сторону: `ssh host 'rm -rf /'` уедет
 *     дословно, ключ разрешения «ssh» спрашивает пользователя;
 *   - не заменяет разрешения: безопасная команда всё равно спросит
 *     пользователя, если на неё нет правила (дефолт — Ask, И2.3).
 */

#include "permission.h"

#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace coder {

/* --- Разобранная команда (И3.2) ---
 *
 * Всё, что нужно проверке, хранится РАЗБРАННЫМ: имя программы, аргументы
 * без кавычек, перенаправления и — главное — вложенные команды. Строка
 * целиком для проверок бесполезна: именно разбор отличает
 * `cat x | sh` (две команды, вторая запрещена) от `echo "a | sh"`
 * (одна команда, внутри кавычек).
 *
 * binary может содержать путь: плагин запускает '/usr/bin/php8.1' и
 * '/usr/local/bin/composer'. Сопоставление с allowlist идёт по имени
 * файла без каталога и без номера версии (php8.1 → php), иначе
 * allowlist пришлось бы пополнять на каждый релиз дистрибутива.
 */
struct ParsedCommand {
    std::string binary;                        /* имя программы, как написано */
    std::vector<std::string> args;             /* аргументы, кавычки сняты */
    std::vector<std::string> redirections;     /* ">file", "2>&1", "<<EOF" */
    std::vector<std::string> env;              /* FOO=bar в начале команды */
    std::vector<ParsedCommand> sub_commands;   /* конвейер, ;, &&, $(), `` */
    /* В команде была подстановка — $( … ), ${ … } или ` … `. Хранится
     * флагом, а не маркером в тексте аргумента: иначе проверка
     * «есть ли подстановка» ловила бы любую строку с фигурными
     * скобками, включая '--format {{.Names}}' у docker. */
    bool has_substitution = false;

    /* Канонический текст для правил и сообщений: «бинарник + аргументы +
     * перенаправления». Именно он сопоставляется с arg_rules и с
     * паттернами разрешений. */
    std::string text() const;
    /* Имя программы без каталога: «/usr/bin/php8.1» → «php8.1». Именно
     * в таком виде оно нужно для шаблона «всегда» (И3.5): паттерн
     * сопоставляется с текстом команды, а текст начинается с имени как
     * она написана, и «php *» не совпало бы с «php8.1 -l index.php».
     * Для сопоставления с таблицами используется variants(). */
    std::string program() const;
    /* Имена, которыми эта программа может называться в таблицах
     * политики: program(), вариант без расширения («mkfs.ext4» → «mkfs»)
     * и варианты без номера версии («php8.1» → «php8», «php»).
     *
     * Один program() тут не годится, и это видно на двух именах сразу:
     * отбрасывание номера версии превращало «php8.1» в «php» (верно), а
     * «mkfs.ext4» — в «mkfs.ext» (обрезано посередине, и запрет на mkfs
     * переставал срабатывать). Перебор вариантов ничего не обрезает. */
    std::vector<std::string> variants() const;
    /* Есть ли аргумент, начинающийся с указанного флага. */
    bool has_flag(const std::string& flag) const;
    /* Первый аргумент, не начинающийся с '-' (без учёта «-C dir»). */
    std::string first_positional() const;
};

/* --- Политика команд (И3.1) ---
 *
 * allowed_binaries — что вообще можно запускать. Всё остальное
 * отклоняется: fail-closed, как и дефолт Ask в И2.3.
 *
 * arg_rules — упорядоченный список правил поверх allowlist, с той же
 * семантикой last match wins, что в core/permission.h. Совпадение идёт
 * по permission (имя программы или «*») и pattern (вилка по
 * каноническому тексту команды). Смысл разделения двух уровней:
 * правило может ОТКРЫТЬ бинарник (пользователь знает, зачем ему
 * `curl` в этом проекте), но не может отменить запрет валидатора
 * (`rm -rf /` запрещён кодом, а не настройкой).
 */
class CommandPolicy {
public:
    /* Дефолты: allowlist программ, запреты по бинарникам, доверенные
     * хосты localhost. Никаких правил из настроек — они приходят
     * сверху (load_user_rules, И3.6). */
    CommandPolicy();

    /* --- Проверка ---
     * Пустая строка = разрешено. Непустая = причина отказа по-русски,
     * в форме отказа инструмента: её читает модель.
     *
     * check()           — полная проверка, включая allowlist бинарников
     *                     и allowlist хостов для curl/wget. Для команд,
     *                     написанных моделью (exec_command).
     * check_assembled() — только запреты, без allowlist бинарников и
     *                     без сетевых хостов. Для команд, собранных
     *                     кодом плагина из аргументов модели. */
    std::string check(const std::string& cmd) const;
    std::string check_assembled(const std::string& cmd) const;

    /* --- Спец-валидаторы (И3.4), вынесены наружу для тестов --- */
    std::string check_git(const ParsedCommand& c) const;
    std::string check_sudo(const ParsedCommand& c) const;
    std::string check_network(const ParsedCommand& c) const;
    std::string check_remove(const ParsedCommand& c) const;
    std::string check_interpreter(const ParsedCommand& c) const;
    std::string check_awk(const ParsedCommand& c) const;
    std::string check_env(const ParsedCommand& c) const;

    /* --- Настройка --- */
    const std::set<std::string>& allowed_binaries() const { return allowed_; }
    bool binary_allowed(const std::string& binary) const;
    /* Настройка — до начала работы агента (иначе гонка с рабочим
     * потоком: единственное исключение — trust_host, см. ниже). */
    void allow_binary(const std::string& binary);
    void deny_binary(const std::string& binary);
    const std::vector<Rule>& arg_rules() const { return arg_rules_; }
    void add_arg_rule(Rule r);
    void add_arg_rule(const std::string& permission, const std::string& pattern,
                      PermissionAction action);
    void clear_arg_rules();

    /* --- Сеть (И3.4) ---
     * curl/wget в exec_command ходят только по доверенным хостам. Не
     * «список плохих», а список допустимых: имя хоста придумать можно
     * любое, и deny-список здесь принципиально не работает. Штатно
     * доверены только localhost — остальное открывается настройкой
     * (Engine подкладывает wp_site_url и deploy_host, И3.6).
     *
     * Обёртка этим НЕ пользуется: проверка web_fetch за веб-инструментом
     * бессмысленна (curl там и есть), но и не мешает — запреты
     * (--config, --upload-file, выгрузка в /etc) действуют везде. */
    void trust_host(const std::string& host);   /* потокобезопасно */
    void clear_trusted_hosts();
    bool host_trusted(const std::string& host) const;
    std::set<std::string> trusted_hosts() const;
    /* Хост из аргумента curl/wget, "" если это не URL с хостом. */
    static std::string host_from_url(const std::string& arg);

    /* --- И3.5: паттерн для кнопки «всегда» ---
     * «Всегда разрешить git status --porcelain» не должно разрешать
     * git push --force, поэтому паттерн = «бинарник + подкоманда»,
     * а не сама команда и не один «git *». */
    std::string always_pattern(const std::string& cmd) const;

private:
    std::string check_one(const ParsedCommand& c, bool strict) const;
    std::string check_list(const std::vector<ParsedCommand>& cmds,
                           bool strict) const;

    std::set<std::string> allowed_;
    std::vector<Rule> arg_rules_;

    /* Единственное поле, меняющееся во время работы агента: Engine
     * подкладывает доверенные хосты при загрузке настроек, пока
     * инструменты уже могут звать check(). Остальное неизменно после
     * конструктора, поэтому гонки нет; здесь — единственный мьютекс в
     * классе, и он никогда не пересекается с state_.mtx (D1) и с
     * PermissionEngine::mtx_ (И2.5): держится только на время
     * копирования множества хостов. */
    mutable std::mutex host_mtx_;
    std::set<std::string> trusted_hosts_;
};

/* --- Разбор команды (И3.2, И3.3) --- */

/* Разобрать строку в список команд верхнего уровня. Разбор рекурсивный:
 * `a | b; $(c && d)` даст три команды с вложенными. */
std::vector<ParsedCommand> parse_command_line(const std::string& cmd);

/* Политика процесса. Одна на плагин: решения не зависят от сессии,
 * доверенные хосты приходят из настроек. Создание — thread-safe
 * (function-local static), обращение — тоже (хосты под мьютексом). */
CommandPolicy& command_policy();

} // namespace coder
