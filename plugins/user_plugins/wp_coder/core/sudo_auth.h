#pragma once

/*
 * sudo_auth.h — запрос пароля sudo у человека и выполнение команды под
 * PTY (И13.10).
 *
 * ЗАЧЕМ. Инструмент `bash` не имеет управляющего терминала: `sudo`
 * запрашивает пароль с /dev/tty, а набранное в терминале приложения до
 * дочернего процесса не доходит. До этого инструмент отказывался
 * выполнять команду и предлагал человеку вручную сделать `sudo -v` в
 * терминале — уже после того, как задача выполнена. Здесь пароль
 * спрашивается окном приложения, а команда выполняется на PTY: sudo
 * получает настоящий терминал, пароль уходит в него через master, и
 * учётные данные кэшируются по tty — поэтому один ввод пароля покрывает
 * ВСЕ sudo внутри команды (в том числе `sudo a && sudo b`, где кэш по
 * ppid не сработал бы).
 *
 * ГРАНИЦЫ СЕКРЕТА.
 *   - пароль никогда не попадает в argv (`ps` его не видит): только в
 *     stdin PTY, который sudo читает с выключенным эхом;
 *   - пароль не попадает в вывод инструмента, чат, лог событий, файл
 *     сессии и spill: фильтр вырезает приглашение, а если пароль всё же
 *     отразился (режим passwd_asterisks/чужой getpass) — заменяет его;
 *   - пароль живёт в SudoRequest ровно от ответа UI до запуска sudo и
 *     затирается после попытки; pending() отдаёт копию БЕЗ пароля;
 *   - в приглашении стоит уникальная метка, поэтому пароль отправляется
 *     только на неё, а не на первый текст «password» в выводе команды.
 *
 * ПОТОКИ. ask() блокирует рабочий поток агента на condvar — ровно как
 * PermissionEngine. Правило блокировок то же: mtx_ этого класса никогда
 * не берётся поверх state_.mtx и не удерживается во время ожидания;
 * касание состояния движка — короткими отдельными захватами.
 */

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace coder {

struct EngineState;

/* Запрос пароля, висящий на человеке. Копия для UI (pending()) приходит
 * уже без password — окно получает id, команду и текст ошибки. */
struct SudoRequest {
    uint64_t id = 0;
    std::string command;   /* команда, которой нужен root (показывается) */
    std::string error;     /* «пароль не принят, попытка 2 из 3» */
    bool decided = false;
    bool allowed = false;
    std::string password;  /* заполняет UI; наружу не отдаётся */
};

/* Итог выполнения команды под sudo. */
struct SudoRunResult {
    /* Команда действительно запускалась (аутентификация прошла). Если
     * false — execute не выполнялся, и в error написана причина. */
    bool executed = false;
    bool prompt_seen = false;   /* sudo действительно показал приглашение */
    bool cancelled = false;
    bool timed_out = false;
    int exit_code = -1;
    size_t total_bytes = 0;
    size_t dropped_bytes = 0;
    double elapsed_ms = 0.0;
    std::string output;      /* хвост вывода, уже без приглашения sudo */
    std::string spill_path;  /* полный вывод, если запрошен */
    std::string error;       /* заполнен, только если !executed */
};

class SudoAuth {
public:
    SudoAuth() = default;

    /* Мост в состояние движка и журнал событий. Без привязки ask()
     * работает, но не может перевести агента в «ожидание пользователя»
     * и не пишет события; тесты обходятся без неё. */
    void bind(EngineState* state, std::function<void(const std::string&)> push);

    /* Спросить пароль и выполнить команду. Блокирует вызывающий поток
     * (у `bash` — рабочий поток агента).
     *
     * command        — команда модели; выполняется под `sh -c`, пароль
     *                  нужен только первому sudo (дальше кэш по tty);
     * timeout_sec    — предел выполнения самой команды (после пароля);
     * cancelled      — признак «пользователь нажал стоп»;
     * on_progress    — живой хвост вывода (как у shell::RunOptions);
     * spill_path     — куда писать полный вывод (необязательно).
     *
     * Возвращает executed=true, если sudo принял пароль и команда
     * запустилась (независимо от её кода возврата). */
    SudoRunResult run(const std::string& command, unsigned timeout_sec,
                      const std::function<bool()>& cancelled,
                      const std::function<void(size_t, const std::string&)>& on_progress = {},
                      const std::string& spill_path = {});

    /* Ответы из UI. reply — пароль; reject — «отмена». false означает,
     * что запроса с таким id уже нет (устарел/отвечен). */
    bool reply(uint64_t id, const std::string& password);
    /* Перегрузка для буфера окна: временная std::string создаётся здесь
     * же и затирается, а не остаётся копией в чужом стеке. */
    bool reply(uint64_t id, const char* password);
    bool reject(uint64_t id);

    /* Снимок ждущих запросов для окна. Пароль в копию НЕ попадает. */
    std::vector<SudoRequest> pending() const;
    size_t pending_count() const;

    /* Отменить ожидающие (стоп задачи, остановка движка). */
    void cancel_all();
    /* Снять отмену перед новой задачей. */
    void clear_cancel();

    /* Сколько ждать ответа человека, мс. 0 — бесконечно (как у
     * PermissionEngine). Тесты ставят конечное значение. */
    void set_wait_timeout_ms(int ms);
    int wait_timeout_ms() const;

    /* Сколько раз спрашивать пароль заново, если sudo его не принял.
     * Больше трёх — это уже перебор, а не опечатка. */
    void set_max_attempts(int n);
    int max_attempts() const;

    /* Предел ожидания ВВОДА ПАРОЛЯ (не команды): человек может искать
     * пароль дольше, чем живёт сама команда. */
    void set_prompt_timeout_ms(int ms);
    int prompt_timeout_ms() const;

    /* Путь к sudo. По умолчанию "sudo"; тесты подставляют заглушку,
     * чтобы не зависеть от настроек машины. */
    void set_sudo_binary(std::string path);
    std::string sudo_binary() const;

private:
    struct AskResult {
        bool granted = false;
        bool cancelled = false;
        bool timed_out = false;
        std::string password;
    };

    AskResult ask(const std::string& command, const std::string& error);

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<SudoRequest> pending_;
    uint64_t next_id_ = 0;
    bool cancelled_ = false;
    int wait_timeout_ms_ = 0;
    int max_attempts_ = 3;
    int prompt_timeout_ms_ = 300000;
    std::string sudo_bin_ = "sudo";
    EngineState* state_ = nullptr;
    std::function<void(const std::string&)> push_;
};

/* Затирание строки: компилятор не должен выбросить запись нулей как
 * «запись в то, что больше не читается». Живёт здесь, а не в shell.h:
 * это про секрет, а не про запуск команд. */
void secure_wipe(std::string& s);

} // namespace coder