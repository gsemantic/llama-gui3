/*
 * test_sudo_auth.cpp — И13.10: пароль sudo спрашивается окном, команда
 * выполняется на PTY, секрет не уходит в вывод.
 *
 * Настоящий sudo здесь не участвует: в temp-каталог кладётся заглушка,
 * которая печатает то же приглашение (-p), читает пароль с терминала и
 * выходит с нужным кодом. Это проверяет ровно то, что написано в
 * sudo_auth.cpp: канал PTY, отправку пароля по метке, повторные попытки,
 * отмену и отсутствие пароля в выводе.
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/sudo_auth.h"

#include <atomic>
#include <cstdlib>
#include <unistd.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using namespace coder;
using namespace std::chrono_literals;

namespace {

struct FakeSudo {
    std::string dir;
    std::string script;

    ~FakeSudo() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

/* Заглушка sudo. mode:
 *   "ok"    — принимает GOOD и запоминает, что пароль уже был (имитация
 *             кэша по tty: второй sudo в той же PTY не спрашивает);
 *   "bad"   — не принимает никакой пароль;
 *   "retry" — отвергает пароль и переспрашивает (как настоящий sudo). */
FakeSudo make_fake_sudo(const std::string& good, const char* mode) {
    FakeSudo f;
    f.dir = (std::filesystem::temp_directory_path() /
             ("wp_sudo_auth_" + std::to_string(::getpid()) + "_" +
              std::to_string(std::rand() % 100000))).string();
    std::filesystem::create_directories(f.dir);
    register_tmp_tree(f.dir);

    const std::string marker = f.dir + "/authed";
    f.script = f.dir + "/sudo";

    std::ofstream out(f.script, std::ios::binary | std::ios::trunc);
    out << "#!/bin/sh\n"
        << "prompt=\"Password:\"\n"
        << "orig=\"$*\"\n"
        << "nonint=0\n"
        << "while [ $# -gt 0 ]; do\n"
        << "  case \"$1\" in\n"
        << "    -p) prompt=\"$2\"; shift 2 ;;\n"
        << "    -v) shift ;;\n"
        << "    -n) nonint=1; shift ;;\n"
        << "    *) shift ;;\n"
        << "  esac\n"
        << "done\n"
        << "printf '%s\\n' \"$prompt\" >> '" << f.dir << "/prompts'\n"
        << "if [ \"$nonint\" = 1 ]; then exit 1; fi\n";
    if (std::string(mode) == "ok") {
        out << "if [ -f '" << marker << "' ]; then\n"
            << "  printf 'RAN %s\\n' \"$orig\"\n"
            << "  exit 0\n"
            << "fi\n";
    }
    if (std::string(mode) == "echo") {
        /* Пароль отражается терминалом: ровно тот случай, ради которого
         * фильтр вырезает его из вывода и spill. */
        out << "stty echo 2>/dev/null\n";
    } else {
        out << "stty -echo 2>/dev/null\n";
    }
    out << "printf '%s' \"$prompt\"\n"
        << "IFS= read -r pw\n"
        << "stty echo 2>/dev/null\n";
    if (std::string(mode) == "ok" || std::string(mode) == "echo") {
        out << "if [ \"$pw\" = '" << good << "' ]; then\n"
            << "  : > '" << marker << "'\n"
            << "  exit 0\n"
            << "fi\n";
    }
    if (std::string(mode) == "retry") {
        /* sudo переспрашивает после неверного пароля. Второй пароль
         * родитель не отправит: он обязан распознать повтор и быстро
         * завершиться, а не ждать таймаут. */
        out << "stty -echo 2>/dev/null\n"
            << "printf '%s' \"$prompt\"\n"
            << "IFS= read -r pw2\n"
            << "stty echo 2>/dev/null\n";
    }
    out << "exit 1\n";
    out.close();
    std::filesystem::permissions(f.script,
                                 std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    return f;
}

struct PathGuard {
    std::string old;
    explicit PathGuard(const std::string& prefix) {
        const char* p = std::getenv("PATH");
        old = p ? p : "";
        setenv("PATH", (prefix + ":" + old).c_str(), 1);
    }
    ~PathGuard() { setenv("PATH", old.c_str(), 1); }
};

/* Ответить из «UI»: подождать запрос и послать пароль/отказ. */
void answer_when_ready(SudoAuth& sa, const std::string& password,
                       bool reject = false) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto pending = sa.pending();
        if (!pending.empty()) {
            if (reject) sa.reject(pending[0].id);
            else sa.reply(pending[0].id, password);
            return;
        }
        std::this_thread::sleep_for(5ms);
    }
}

} // namespace

TEST(sudo_auth_password_from_window_runs_command) {
    const std::string good = "s3cret-pass";
    FakeSudo fake = make_fake_sudo(good, "ok");

    SudoAuth sa;
    sa.set_sudo_binary(fake.script);
    sa.set_max_attempts(1);
    sa.set_prompt_timeout_ms(5000);

    SudoRunResult r;
    std::thread worker([&] { r = sa.run("echo COMMAND_RAN", 10, {}); });
    answer_when_ready(sa, good);
    worker.join();

    ASSERT_TRUE(r.executed);
    ASSERT_EQ(r.exit_code, 0);
    ASSERT_TRUE(r.output.find("COMMAND_RAN") != std::string::npos);
    /* Пароль не должен попасть в вывод даже если бы wrapping его
     * напечатал: фильтр заменяет его на ***. */
    ASSERT_TRUE(r.output.find(good) == std::string::npos);
    ASSERT_TRUE(r.output.find("Password:") == std::string::npos);
}

TEST(sudo_auth_wrong_password_is_reported_not_hung) {
    FakeSudo fake = make_fake_sudo("right", "bad");

    SudoAuth sa;
    sa.set_sudo_binary(fake.script);
    sa.set_max_attempts(2);
    sa.set_prompt_timeout_ms(5000);

    SudoRunResult r;
    std::thread worker([&] { r = sa.run("echo NEVER", 5, {}); });

    /* Каждая попытка — новый запрос; отвечаем неверным паролем. */
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    int answered = 0;
    while (std::chrono::steady_clock::now() < deadline && answered < 2) {
        const auto pending = sa.pending();
        if (!pending.empty()) {
            sa.reply(pending[0].id, "wrong");
            ++answered;
        }
        std::this_thread::sleep_for(5ms);
    }
    worker.join();

    ASSERT_TRUE(!r.executed);
    ASSERT_TRUE(!r.cancelled);
    ASSERT_TRUE(r.error.find("2") != std::string::npos);
    ASSERT_TRUE(r.output.find("NEVER") == std::string::npos);
}

TEST(sudo_auth_reject_does_not_execute) {
    FakeSudo fake = make_fake_sudo("good", "ok");

    SudoAuth sa;
    sa.set_sudo_binary(fake.script);
    sa.set_prompt_timeout_ms(5000);

    SudoRunResult r;
    std::thread worker([&] { r = sa.run("echo NEVER", 5, {}); });
    answer_when_ready(sa, "", /*reject=*/true);
    worker.join();

    ASSERT_TRUE(!r.executed);
    ASSERT_TRUE(!r.cancelled);
    ASSERT_TRUE(r.output.find("NEVER") == std::string::npos);
}

TEST(sudo_auth_cancel_wakes_waiting_worker) {
    FakeSudo fake = make_fake_sudo("good", "ok");

    SudoAuth sa;
    sa.set_sudo_binary(fake.script);
    sa.set_prompt_timeout_ms(5000);
    sa.set_wait_timeout_ms(0);   /* ждать, пока не отменят */

    SudoRunResult r;
    std::thread worker([&] { r = sa.run("echo NEVER", 5, {}); });

    /* Дать воркеру дойти до ожидания, затем «стоп». */
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline &&
           sa.pending_count() == 0) {
        std::this_thread::sleep_for(5ms);
    }
    sa.cancel_all();
    worker.join();

    ASSERT_TRUE(!r.executed);
    ASSERT_TRUE(r.cancelled);
}

TEST(sudo_auth_pending_snapshot_has_no_password) {
    FakeSudo fake = make_fake_sudo("good", "ok");

    SudoAuth sa;
    sa.set_sudo_binary(fake.script);
    sa.set_prompt_timeout_ms(5000);

    SudoRunResult r;
    std::thread worker([&] { r = sa.run("echo OK", 5, {}); });

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    bool saw_password = false;
    bool checked = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto pending = sa.pending();
        if (!pending.empty()) {
            checked = true;
            if (!pending[0].password.empty()) saw_password = true;
            sa.reply(pending[0].id, "good");
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    worker.join();

    ASSERT_TRUE(checked);
    ASSERT_TRUE(!saw_password);
    ASSERT_TRUE(r.executed);
}

TEST(sudo_auth_one_prompt_covers_two_sudo_in_command) {
    const std::string good = "s3cret";
    FakeSudo fake = make_fake_sudo(good, "ok");

    SudoAuth sa;
    sa.set_sudo_binary(fake.script);
    sa.set_max_attempts(1);
    sa.set_prompt_timeout_ms(5000);

    SudoRunResult r;
    /* Внутренние `sudo` в команде ищутся по PATH, поэтому заглушка
     * должна быть видна и им, а не только внешнему вызову -v. */
    PathGuard path(fake.dir);
    std::thread worker([&] {
        r = sa.run("sudo echo ONE && sudo echo TWO", 10, {});
    });
    answer_when_ready(sa, good);
    worker.join();

    /* Если бы второй sudo снова спросил пароль, отправка была бы только
     * одна (SudoAuth::run_pty помнит sent), и команда повисла бы до
     * таймаута. Успех означает, что одного ввода хватило на всю PTY. */
    ASSERT_TRUE(r.executed);
    ASSERT_EQ(r.exit_code, 0);
    ASSERT_TRUE(r.output.find("ONE") != std::string::npos);
    ASSERT_TRUE(r.output.find("TWO") != std::string::npos);
    ASSERT_TRUE(r.output.find(good) == std::string::npos);
}
TEST(sudo_auth_second_prompt_fails_fast) {
    FakeSudo fake = make_fake_sudo("good", "retry");

    SudoAuth sa;
    sa.set_sudo_binary(fake.script);
    sa.set_max_attempts(1);
    sa.set_prompt_timeout_ms(10000);

    SudoRunResult r;
    const auto start = std::chrono::steady_clock::now();
    std::thread worker([&] { r = sa.run("echo NEVER", 5, {}); });
    answer_when_ready(sa, "wrong");
    worker.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    ASSERT_TRUE(!r.executed);
    ASSERT_TRUE(r.prompt_seen);
    /* Повторное приглашение — конец: sudo сам ограничен тремя попытками,
     * и ждать его молчания до таймаута значит «завис на пять минут». */
    ASSERT_TRUE(elapsed < std::chrono::milliseconds(5000));
    ASSERT_TRUE(r.output.find("NEVER") == std::string::npos);
}

TEST(sudo_auth_spill_never_contains_password_or_prompt) {
    const std::string good = "s3cret-pass";
    FakeSudo fake = make_fake_sudo(good, "echo");

    SudoAuth sa;
    sa.set_sudo_binary(fake.script);
    sa.set_max_attempts(1);
    sa.set_prompt_timeout_ms(5000);

    const std::string spill = fake.dir + "/spill.log";
    SudoRunResult r;
    std::thread worker([&] {
        r = sa.run("echo COMMAND_RAN", 10, {}, {}, spill);
    });
    answer_when_ready(sa, good);
    worker.join();

    ASSERT_TRUE(r.executed);
    ASSERT_TRUE(r.output.find("COMMAND_RAN") != std::string::npos);
    ASSERT_TRUE(r.output.find(good) == std::string::npos);

    std::ifstream f(spill, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    ASSERT_TRUE(text.find("COMMAND_RAN") != std::string::npos);
    /* Секрет не должен попасть в файл, который читает модель: PTY
     * отразил пароль (stty echo), и фильтр обязан его заменить. */
    ASSERT_TRUE(text.find(good) == std::string::npos);
    ASSERT_TRUE(text.find("wp_coder_sudo_password:") == std::string::npos);
}

TEST(sudo_auth_prompt_is_unique_per_run) {
    FakeSudo fake = make_fake_sudo("good", "bad");

    SudoAuth sa;
    sa.set_sudo_binary(fake.script);
    sa.set_max_attempts(1);
    sa.set_prompt_timeout_ms(5000);

    for (int i = 0; i < 2; ++i) {
        SudoRunResult r;
        std::thread worker([&] { r = sa.run("echo NEVER", 5, {}); });
        answer_when_ready(sa, "wrong");
        worker.join();
        ASSERT_TRUE(!r.executed);
    }

    std::ifstream f(fake.dir + "/prompts");
    std::vector<std::string> prompts;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty()) prompts.push_back(line);
    }
    ASSERT_TRUE(prompts.size() >= 2);
    /* Постоянная метка была бы угадываемой: агент читает исходники и
     * может напечатать её, чтобы пароль ушёл в команду. */
    ASSERT_TRUE(prompts[0] != prompts[1]);
    ASSERT_TRUE(prompts[0].rfind("wp_coder_sudo_password:", 0) == 0);
    ASSERT_TRUE(prompts[1].rfind("wp_coder_sudo_password:", 0) == 0);
}
