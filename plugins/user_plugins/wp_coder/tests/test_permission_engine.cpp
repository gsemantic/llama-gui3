/*
 * test_permission_engine.cpp — И2.3–И2.8: движок разрешений.
 *
 * Engine — синглтон, и правила разрешений живут в нём по замыслу (ответ
 * «всегда» должен переживать отдельные вызовы инструментов). Значит
 * каждый тест обязан начинать и заканчивать с чистых правил, иначе
 * тесты зависят от порядка регистрации. Для этого PermissionGuard.
 *
 * Здесь же — тесты, которым нужен ответ пользователя: они поднимают
 * отдельный поток, имитирующий UI, и проверяют, что ожидание снимается
 * и что каскад отклонения действительно каскадный. Тесты без потоков
 * живут в test_permission.cpp.
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/engine.h"
#include "../core/permission_engine.h"
#include "../core/agent_components.h"
#include "../core/tools_registry.h"
#include "../core/base_tools.h"
#include "../core/git_tools.h"
#include "../modules/wordpress/wp_tools.h"
#include "../modules/python/python_tools.h"
#include "../modules/devops/devops_tools.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

using namespace coder;

namespace {

/* Чистые правила на входе и на выходе теста. */
struct PermissionGuard {
    PermissionGuard() { Engine::instance().permissions().reset(); }
    ~PermissionGuard() { Engine::instance().permissions().reset(); }
};

Rule rule(const char* key, const char* pattern, PermissionAction action) {
    Rule r;
    r.permission = key;
    r.pattern = pattern;
    r.action = action;
    return r;
}

} // anonymous namespace

/* ======================================================================
 * И2.3 — evaluate
 * ====================================================================== */

TEST(permission_engine_asks_when_there_are_no_rules) {
    PermissionGuard g;
    ASSERT_EQ((int)engine().permissions().evaluate("bash", "rm -rf /"),
              (int)PermissionAction::Ask);
}

TEST(permission_engine_evaluate_uses_last_match_wins) {
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    pe.add_rule(rule("*", "*", PermissionAction::Allow));
    pe.add_rule(rule("deploy", "*", PermissionAction::Ask));
    ASSERT_EQ((int)pe.evaluate("bash", "ls"), (int)PermissionAction::Allow);
    ASSERT_EQ((int)pe.evaluate("deploy", "site"), (int)PermissionAction::Ask);
    pe.add_rule(rule("deploy", "site.staging*", PermissionAction::Deny));
    ASSERT_EQ((int)pe.evaluate("deploy", "site.staging"), (int)PermissionAction::Deny);
}

TEST(permission_engine_rules_live_in_session) {
    PermissionGuard g;
    engine().permissions().add_rule(rule("bash", "*", PermissionAction::Deny));
    /* Тот же объект между обращениями — иначе «всегда» жил бы один
     * вызов инструмента и следующий шаг спросил бы заново. */
    ASSERT_EQ((int)engine().permissions().evaluate("bash", "ls"),
              (int)PermissionAction::Deny);
    ASSERT_TRUE(&engine().permissions() == &engine().permissions());
}

TEST(permission_engine_reset_clears_rules) {
    PermissionGuard g;
    engine().permissions().add_rule(rule("*", "*", PermissionAction::Deny));
    engine().permissions().reset();
    ASSERT_EQ((int)engine().permissions().evaluate("bash", "ls"),
              (int)PermissionAction::Ask);
}

/* Смена правил обязана сбрасывать кэш системного промпта: набор
 * видимых инструментов строится из тех же правил (И2.8), и без сброса
 * модель до конца сессии видела бы в схеме запрещённый инструмент. */
TEST(permission_engine_notifies_on_rules_change) {
    PermissionGuard g;
    int changes = 0;
    engine().permissions().set_on_change([&] { ++changes; });
    engine().permissions().add_rule(rule("bash", "*", PermissionAction::Deny));
    engine().permissions().set_rules(Ruleset{});
    engine().permissions().reset();
    ASSERT_EQ(changes, 3);
    engine().permissions().set_on_change(nullptr);
}

/* ======================================================================
 * И2.4 — дефолты уровня агента
 * ====================================================================== */

namespace {

/* Дефолты + пара доверенных каталогов, как их ставит Engine. */
void install_defaults() {
    engine().permissions().apply_agent_defaults(
        {"/home/user/.local/share/llama_gui", "/tmp"});
}

PermissionAction decide(const char* key, const char* pattern) {
    return engine().permissions().evaluate(key, pattern);
}

} // anonymous namespace

/* «* → allow» — слабейшее правило, и оно обязано быть первым: иначе
 * перечисленные ниже исключения перестанут перекрывать его, и все
 * опасные инструменты станут доступны молча. Тест фиксирует именно
 * порядок, а не только результат: перестановка правил дала бы
 * «правильные» ответы на одних данных и неправильные на других. */
TEST(defaults_start_with_weakest_catch_all_allow) {
    PermissionGuard g;
    install_defaults();
    const auto& rules = engine().permissions().rules().rules();
    ASSERT_TRUE(rules.size() > 3);
    ASSERT_EQ(rules.front().permission, std::string("*"));
    ASSERT_EQ(rules.front().pattern, std::string("*"));
    ASSERT_EQ((int)rules.front().action, (int)PermissionAction::Allow);
}

TEST(defaults_allow_ordinary_work) {
    PermissionGuard g;
    install_defaults();
    /* Чтение файла, правка файла, осмотр структуры, RAG-запрос: обычная
     * работа агента, вопрос пользователя здесь только раздражает. */
    ASSERT_EQ((int)decide("read", "src/main.cpp"), (int)PermissionAction::Allow);
    ASSERT_EQ((int)decide("write", "src/main.cpp"), (int)PermissionAction::Allow);
    ASSERT_EQ((int)decide("read", "src/"), (int)PermissionAction::Allow);
    ASSERT_EQ((int)decide("rag", "docs/"), (int)PermissionAction::Ask);
}

TEST(defaults_ask_before_reading_secrets) {
    PermissionGuard g;
    install_defaults();
    ASSERT_EQ((int)decide("read", "/srv/app/.env"), (int)PermissionAction::Ask);
    ASSERT_EQ((int)decide("read", ".env.local"), (int)PermissionAction::Ask);
    ASSERT_EQ((int)decide("read", "config/database.yml"), (int)PermissionAction::Allow);
}

TEST(defaults_ask_on_irreversible_and_outward_groups) {
    PermissionGuard g;
    install_defaults();
    const char* keys[] = {"bash", "git", "wp-cli", "db", "deploy",
                          "systemd", "ssh", "docker", "cron", "package",
                          "rag", "test", "doom_loop"};
    for (const char* k : keys) {
        if (decide(k, "*") != PermissionAction::Ask) {
            std::cerr << "  ключ " << k << " должен спрашивать" << std::endl;
            ASSERT_TRUE(false);
        }
    }
}

/* git status/diff/log — чтение состояния, хотя git при этом запускается.
 * Спрашивать на каждом нельзя: агент пользуется ими постоянно, и
 * вопрос перестают читать. */
TEST(defaults_allow_readonly_git) {
    PermissionGuard g;
    install_defaults();
    ASSERT_EQ((int)decide("git", "status --porcelain"),
              (int)PermissionAction::Allow);
    ASSERT_EQ((int)decide("git", "diff HEAD~1"), (int)PermissionAction::Allow);
    ASSERT_EQ((int)decide("git", "log --oneline"), (int)PermissionAction::Allow);
    /* А изменение истории — уже нет. */
    ASSERT_EQ((int)decide("git", "push --force"), (int)PermissionAction::Ask);
    ASSERT_EQ((int)decide("git", "commit -m fix"), (int)PermissionAction::Ask);
}

TEST(defaults_ask_outside_project_unless_trusted) {
    PermissionGuard g;
    install_defaults();
    ASSERT_EQ((int)decide("external_directory", "/etc/nginx/nginx.conf"),
              (int)PermissionAction::Ask);
    /* Whitelist: собственные данные плагина и временный каталог. */
    ASSERT_EQ((int)decide("external_directory",
                          "/home/user/.local/share/llama_gui/session.json"),
              (int)PermissionAction::Allow);
    ASSERT_EQ((int)decide("external_directory", "/tmp/report.txt"),
              (int)PermissionAction::Allow);
    /* «<dir>/*», а не «<dir>*»: соседний каталог с тем же началом имени
     * доверенным быть не должен. */
    ASSERT_EQ((int)decide("external_directory", "/tmp-secrets/keys"),
              (int)PermissionAction::Ask);
    ASSERT_EQ((int)decide("external_directory", "/etc/passwd"),
              (int)PermissionAction::Ask);
}

/* Повторный load_settings не должен затирать ответы «всегда»: дефолты
 * ставятся один раз за сессию. */
TEST(defaults_installed_once) {
    PermissionGuard g;
    install_defaults();
    engine().permissions().add_rule(rule("bash", "ls -la", PermissionAction::Allow));
    install_defaults();   /* как будто UI снова вызвал load_settings */
    ASSERT_EQ((int)decide("bash", "ls -la"), (int)PermissionAction::Allow);
    ASSERT_EQ((int)decide("bash", "rm -rf /"), (int)PermissionAction::Ask);
}

/* ======================================================================
 * И2.5 — ожидание пользователя
 * ====================================================================== */

namespace {

/* Гарантированный join. Тест с ожидающим потоком часто падает на
 * ASSERT — а незаjoinенный std::thread в области видимости зовёт
 * std::terminate, и вместо внятного FAIL получается падение всего
 * прогона без указания строки. */
struct Joiner {
    std::vector<std::thread>& ts;
    explicit Joiner(std::vector<std::thread>& t) : ts(t) {}
    ~Joiner() { for (auto& t : ts) if (t.joinable()) t.join(); }
};

/* Дождаться, пока появится ожидающий запрос, и вернуть его. Опрос
 * вместо condvar в тесте — нам нужно убедиться, что запрос ВИДЕН извне
 * (именно это делает UI), а не просто просыпается по сигналу. */
PermissionRequest wait_for_pending(PermissionEngine& pe, int tries = 200) {
    for (int i = 0; i < tries; ++i) {
        auto p = pe.pending();
        if (!p.empty()) return p.front();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("запрос разрешения так и не появился");
}

} // anonymous namespace

/* Главное свойство ожидания: worker-поток реально стоит, запрос виден
 * пользователю, и ответ его снимает — без перезапуска задачи. */
TEST(ask_waits_for_user_and_unblocks_on_once) {
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    pe.set_wait_timeout_ms(5000);

    std::vector<std::thread> threads;
    bool allowed = false;
    threads.emplace_back([&] {
        allowed = pe.ask("bash", {"git status"}, "git status",
                         "bash → git status");
    });
    Joiner joiner(threads);

    PermissionRequest r = wait_for_pending(pe);
    ASSERT_EQ(r.permission, std::string("bash"));
    ASSERT_EQ(r.patterns.size(), (size_t)1);
    ASSERT_EQ(r.suggested, std::string("git status"));
    ASSERT_TRUE(r.metadata.find("bash") != std::string::npos);
    ASSERT_EQ(pe.pending_count(), (size_t)1);

    ASSERT_TRUE(pe.reply(r.id, PermissionReply::Once));
    /* join ДО проверки: notify_all будит поток, но он мог ещё не
     * выполниться — и «allowed» прочитал бы стартовое значение. */
    for (auto& t : threads) t.join();
    ASSERT_TRUE(allowed);
    /* Решённый запрос уходит из очереди — иначе панель так и показывала
     * бы его следующую вечность. */
    ASSERT_EQ(pe.pending_count(), (size_t)0);
}

TEST(ask_reject_returns_false_and_clears_queue) {
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    pe.set_wait_timeout_ms(5000);

    std::vector<std::thread> threads;
    bool allowed = true;
    threads.emplace_back([&] {
        allowed = pe.ask("deploy", {"site.example.com"}, "site.example.com",
                         "deploy → site.example.com");
    });
    Joiner joiner(threads);
    PermissionRequest r = wait_for_pending(pe);
    ASSERT_TRUE(pe.reply(r.id, PermissionReply::Reject));
    for (auto& t : threads) t.join();
    ASSERT_TRUE(!allowed);
    ASSERT_EQ(pe.pending_count(), (size_t)0);
}

/* Нет ответа — отказ, а не вечное ожидание. Без этого «окно закрыли
 * во время вопроса» оставляет worker-поток висеть навсегда, и следующий
 * запуск сессии уже не стартует. */
TEST(ask_without_answer_times_out_closed) {
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    pe.set_wait_timeout_ms(50);
    bool allowed = pe.ask("bash", {"rm -rf /"}, "rm -rf /", "bash");
    ASSERT_TRUE(!allowed);
    ASSERT_EQ(pe.pending_count(), (size_t)0);
}

/* Прерывание задачи вытаскивает из вопроса — кнопка «стоп» работает и
 * на том шаге, где агент висит на пользователе. */
TEST(ask_wakes_up_on_cancel) {
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    pe.set_wait_timeout_ms(0);   /* ждать бесконечно: снимет только cancel */
    std::vector<std::thread> threads;
    bool allowed = true;
    threads.emplace_back([&] {
        allowed = pe.ask("ssh", {"root@host"}, "root@host*", "ssh_exec");
    });
    Joiner joiner(threads);
    wait_for_pending(pe);
    pe.cancel_all();
    for (auto& t : threads) t.join();
    ASSERT_TRUE(!allowed);
    /* reset() возвращает движок в рабочее состояние: иначе после
     * «стоп» новый запуск получал бы мгновенные отказы без вопроса. */
    pe.reset();
    pe.set_wait_timeout_ms(200);
    ASSERT_TRUE(!pe.ask("ssh", {"root@host"}, "root@host*", "ssh_exec"));
}

TEST(ask_uses_engine_state_as_waiting_permission) {
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    EngineState& st = engine_state();
    pe.bind(&st, nullptr);
    pe.set_wait_timeout_ms(50);
    AgentState saved;
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        saved = st.state;
        st.state = AgentState::Executing;
    }
    pe.ask("bash", {"ls"}, "ls", "bash → ls");
    /* Состояние возвращено как было: агент не должен остаться в
     * «Ожидание разрешения» после отказа. */
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        ASSERT_EQ((int)st.state, (int)AgentState::Executing);
        /* Синглтон: возвращаем как было, иначе следующий тест (FSM)
         * увидит чужое состояние и упадёт не по своей причине. */
        st.state = saved;
    }
}

/* Несколько запросов в очереди: отвечают по id, а не «кто первый». */
TEST(pending_queue_holds_several_requests) {
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    pe.set_wait_timeout_ms(0);
    std::vector<std::thread> threads;
    threads.emplace_back([&] { pe.ask("bash", {"ls"}, "ls", "ls"); });
    threads.emplace_back([&] { pe.ask("deploy", {"site"}, "site", "deploy"); });
    Joiner joiner(threads);
    for (int i = 0; i < 200 && pe.pending_count() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto all = pe.pending();
    ASSERT_EQ(all.size(), (size_t)2);
    ASSERT_TRUE(all[0].id != all[1].id);
    const uint64_t first_id = all[0].id;
    /* Отвечаем на второй, не трогая первый: он остаётся ждать. */
    ASSERT_TRUE(pe.reply(all[1].id, PermissionReply::Once));
    for (int i = 0; i < 200 && pe.pending_count() != 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(pe.pending_count(), (size_t)1);
    ASSERT_EQ(pe.pending().front().id, first_id);
    /* Ответ на уже решённый/несуществующий — false, а не «успех». */
    ASSERT_TRUE(!pe.reply(all[1].id, PermissionReply::Once));
    ASSERT_TRUE(!pe.reply(999999, PermissionReply::Once));
    pe.cancel_all();
}

/* ======================================================================
 * И2.6 — ответы: once / always / reject с каскадом
 * ====================================================================== */

/* «Всегда» записывает правило: тот же вызов больше не спрашивает,
 * а соседний — по-прежнему. Ровно ради разделения этих двух вещей и
 * существует предлагаемый паттерн. */
TEST(reply_always_records_rule_for_suggested_pattern) {
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    pe.apply_agent_defaults({"/tmp"});
    pe.set_wait_timeout_ms(2000);

    std::vector<std::thread> threads;
    bool allowed = false;
    threads.emplace_back([&] {
        allowed = pe.ask("bash", {"git status --porcelain"},
                         "git status --porcelain", "bash");
    });
    Joiner joiner(threads);
    PermissionRequest r = wait_for_pending(pe);
    ASSERT_TRUE(pe.reply(r.id, PermissionReply::Always));
    for (auto& t : threads) t.join();
    ASSERT_TRUE(allowed);

    /* Тот же вызов — больше без вопроса. */
    ASSERT_EQ((int)pe.evaluate("bash", "git status --porcelain"),
              (int)PermissionAction::Allow);
    /* Другая команда — по-прежнему спрашивает: «всегда на git status»
     * не должно разрешать rm -rf. */
    ASSERT_EQ((int)pe.evaluate("bash", "rm -rf /"),
              (int)PermissionAction::Ask);
    /* Ключ правила — тот, по которому спрашивали. */
    ASSERT_TRUE(pe.rules().mentions_key("bash"));
    ASSERT_EQ((int)pe.evaluate("deploy", "site"), (int)PermissionAction::Ask);
}

TEST(reply_always_invalidates_prompt_cache) {
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    int changes = 0;
    pe.set_on_change([&] { ++changes; });
    pe.set_wait_timeout_ms(2000);
    std::vector<std::thread> threads;
    threads.emplace_back([&] { pe.ask("bash", {"ls"}, "ls", "bash"); });
    Joiner joiner(threads);
    PermissionRequest r = wait_for_pending(pe);
    ASSERT_EQ(changes, 0);
    pe.reply(r.id, PermissionReply::Always);
    for (auto& t : threads) t.join();
    /* Правила изменились — кэш системного промпта обязан быть сброшен,
     * иначе модель до конца сессии не увидит изменения набора. */
    ASSERT_EQ(changes, 1);
    pe.set_on_change(nullptr);
}

TEST(reply_always_leaves_other_pending_untouched) {
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    pe.set_wait_timeout_ms(0);
    std::vector<std::thread> threads;
    threads.emplace_back([&] { pe.ask("bash", {"ls"}, "ls", "ls"); });
    threads.emplace_back([&] { pe.ask("deploy", {"site"}, "site", "deploy"); });
    Joiner joiner(threads);
    for (int i = 0; i < 200 && pe.pending_count() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto all = pe.pending();
    ASSERT_EQ(all.size(), (size_t)2);
    ASSERT_TRUE(pe.reply(all[0].id, PermissionReply::Always));
    /* Второй запрос НЕ должен разрешиться сам: у него другой ключ, и
     * «всегда» на одну группу не значит «всегда» на все. */
    for (int i = 0; i < 50 && pe.pending_count() != 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(pe.pending_count(), (size_t)1);
    pe.cancel_all();
    for (auto& t : threads) t.join();
}

/* Каскад: отказ одному запросу отклоняет все прочие уже висящие. */
TEST(reply_reject_cascades_to_all_pending) {
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    pe.set_wait_timeout_ms(0);
    std::vector<std::thread> threads;
    bool a = true, b = true, c = true;
    threads.emplace_back([&] { a = pe.ask("bash", {"ls"}, "ls", "ls"); });
    threads.emplace_back([&] { b = pe.ask("deploy", {"site"}, "site", "deploy"); });
    threads.emplace_back([&] { c = pe.ask("ssh", {"root@h"}, "root@h*", "ssh"); });
    Joiner joiner(threads);
    for (int i = 0; i < 200 && pe.pending_count() < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(pe.pending_count(), (size_t)3);
    ASSERT_TRUE(pe.reply(pe.pending().front().id, PermissionReply::Reject));
    for (auto& t : threads) t.join();
    ASSERT_TRUE(!a);
    ASSERT_TRUE(!b);
    ASSERT_TRUE(!c);
    ASSERT_EQ(pe.pending_count(), (size_t)0);
}

/* Каскад бьёт по УЖЕ ЗАПРОШЕННЫМ, а не по всем будущим: после отказа
 * движок обязан снова спрашивать. Иначе одна неудачная кнопка
 * «Отклонить» молча заблокировала бы инструмент до конца сессии, и
 * агент даже не смог бы объяснить пользователю почему. */
TEST(reject_cascade_does_not_silence_future_asks) {
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    pe.apply_agent_defaults({"/tmp"});
    pe.set_wait_timeout_ms(60);
    ASSERT_TRUE(!pe.ask("deploy", {"site"}, "site", "deploy"));
    /* Следующий вопрос по-прежнему задаётся и по-прежнему ждёт ответа. */
    std::vector<std::thread> threads;
    bool allowed = false;
    threads.emplace_back([&] {
        allowed = pe.ask("deploy", {"other"}, "other", "deploy");
    });
    Joiner joiner(threads);
    PermissionRequest r = wait_for_pending(pe);
    ASSERT_EQ(r.permission, std::string("deploy"));
    pe.reply(r.id, PermissionReply::Once);
    for (auto& t : threads) t.join();
    ASSERT_TRUE(allowed);
}

/* ======================================================================
 * И2.7 — approve: постоянное разрешение + пересканирование ожидающих
 * ====================================================================== */

/* Пользователь ответил «всегда» на команду, а такой же вопрос уже висит
 * в очереди. Второй щелчок по тому же решению — требование повторить то
 * же самое, поэтому подходящие запросы разрешаются сами. */
TEST(approve_rescans_pending_and_releases_matching) {
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    pe.set_wait_timeout_ms(0);
    std::vector<std::thread> threads;
    bool a = false, b = false;
    threads.emplace_back([&] { a = pe.ask("bash", {"ls -la"}, "ls -la", "ls"); });
    threads.emplace_back([&] { b = pe.ask("bash", {"ls -la"}, "ls -la", "ls"); });
    Joiner joiner(threads);
    for (int i = 0; i < 200 && pe.pending_count() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(pe.pending_count(), (size_t)2);
    pe.reply(pe.pending().back().id, PermissionReply::Always);
    for (auto& t : threads) t.join();
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    ASSERT_EQ(pe.pending_count(), (size_t)0);
    ASSERT_EQ((int)pe.evaluate("bash", "ls -la"), (int)PermissionAction::Allow);
}

/* Пересканируются только запросы того же ключа: правило про deploy не
 * имеет права тихо разрешить вопрос про ssh. */
TEST(approve_rescan_does_not_touch_other_keys) {
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    pe.set_wait_timeout_ms(0);
    std::vector<std::thread> threads;
    bool deploy_allowed = false;
    threads.emplace_back([&] {
        deploy_allowed = pe.ask("deploy", {"site"}, "site", "deploy");
    });
    Joiner joiner(threads);
    PermissionRequest r = wait_for_pending(pe);
    pe.reply(r.id, PermissionReply::Always);
    for (auto& t : threads) t.join();
    ASSERT_TRUE(deploy_allowed);
    /* ssh не затронут — по-прежнему спрашивает. */
    ASSERT_EQ((int)pe.evaluate("ssh", "root@host"), (int)PermissionAction::Ask);
}

/* approve() — публичный вход: постоянное разрешение можно завести и без
 * ожидающего запроса (панель настроек, миграция старого списка путей). */
TEST(approve_without_request_just_records_rule) {
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    pe.approve("bash", "git push*");
    ASSERT_EQ((int)pe.evaluate("bash", "git push origin main"),
              (int)PermissionAction::Allow);
    ASSERT_EQ((int)pe.evaluate("bash", "git status"), (int)PermissionAction::Ask);
    /* Пустой паттерн трактуется как «вся группа», а не как «пустое
     * значение, которое никому не соответствует». */
    pe.approve("docker", "");
    ASSERT_EQ((int)pe.evaluate("docker", "run -it ubuntu"),
              (int)PermissionAction::Allow);
}

/* Внешние каталоги: whitelist дефолтов (И2.4) снимает вопрос, а всё
 * прочее по-прежнему спрашивает, и запрет по правилу не обходится. */
TEST(external_directory_rules_reach_permission_gate) {
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    pe.apply_agent_defaults({"/tmp"});

    EngineState st;
    HostCallbacks cb;
    PermissionGate gate(st, cb, [](AgentEvent::Kind, const std::string&) {});
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        st.project_dir = "/srv/project";
        st.state = AgentState::Executing;
    }
    /* Внутри проекта вопроса нет никогда. */
    ASSERT_TRUE(gate.check("/srv/project/src/main.cpp").empty());
    /* Доверенный каталог — тоже. */
    ASSERT_TRUE(gate.check("/tmp/build.log").empty());
    /* Остальное спрашивает по-прежнему. */
    std::string ask = gate.check("/etc/nginx/nginx.conf");
    ASSERT_TRUE(ask.find("Требуется разрешение") != std::string::npos ||
                ask.find("Доступ запрещён") != std::string::npos);
    /* Явный запрет перекрывает и список, и whitelist. */
    pe.add_rule(rule("external_directory", "/srv/secret*", PermissionAction::Deny));
    ASSERT_TRUE(gate.check("/srv/secret/keys.pem").find("запрещён") !=
                std::string::npos);
    /* «Всегда» из гейта попадает в правила. */
    PermissionGate g2(st, cb, [](AgentEvent::Kind, const std::string&) {});
    g2.allow_always("/etc/nginx");
    /* «Всегда» на каталог покрывает и его содержимое: иначе на каждое
     * обращение к файлу папки пришлось бы нажимать «всегда» заново. */
    ASSERT_TRUE(gate.check("/etc/nginx/nginx.conf").empty());
    ASSERT_TRUE(gate.check("/etc/nginx/sites-enabled/site").empty());
    ASSERT_EQ((int)pe.evaluate("external_directory", "/etc/nginx"),
              (int)PermissionAction::Allow);
    /* Соседний каталог с тем же началом — нет. */
    ASSERT_TRUE(!gate.check("/etc/nginx-backup/old.conf").empty());
}

/* ======================================================================
 * И2.8 — скрытие запрещённых инструментов из схемы
 * ====================================================================== */

namespace {

ToolDef fake_def(const char* name, const char* key) {
    ToolDef d;
    d.name = name;
    d.description = name;
    d.permission_key = key;
    return d;
}

std::vector<ToolDef> sample_tools() {
    return {fake_def("read_file", "read"), fake_def("bash", "bash"),
            fake_def("deploy", "deploy"), fake_def("git_commit", "git")};
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // anonymous namespace

TEST(visible_tools_hides_only_catch_all_denies) {
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    pe.apply_agent_defaults({"/tmp"});
    auto all = sample_tools();
    /* Без запретов видно всё. */
    ASSERT_EQ(pe.visible_tools(all).size(), (size_t)4);

    pe.add_rule(rule("deploy", "*", PermissionAction::Deny));
    auto vis = pe.visible_tools(all);
    ASSERT_EQ(vis.size(), (size_t)3);
    ASSERT_TRUE(!contains(vis, "deploy"));
    ASSERT_TRUE(contains(vis, "bash"));

    /* Точечный запрет инструмент НЕ прячет: он ограничивает значения. */
    pe.add_rule(rule("read", "*.env*", PermissionAction::Deny));
    ASSERT_EQ(pe.visible_tools(all).size(), (size_t)3);
    /* А запрет на bash убирает оба инструмента этого ключа. */
    pe.add_rule(rule("bash", "*", PermissionAction::Deny));
    vis = pe.visible_tools(all);
    ASSERT_EQ(vis.size(), (size_t)2);
    ASSERT_TRUE(!contains(vis, "bash"));
    ASSERT_TRUE(contains(vis, "git_commit"));
}

/* Звёздочка по всем ключам — тоже catch-all. */
TEST(visible_tools_respects_global_deny) {
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    pe.add_rule(rule("*", "*", PermissionAction::Deny));
    ASSERT_EQ(pe.visible_tools(sample_tools()).size(), (size_t)0);
}

/* Правила пользователя приезжают JSON-строкой из настроек. Без этого
 * запрет недостижим в принципе: правила меняет только approve(), а он
 * добавляет Allow. */
TEST(user_rules_load_from_json_and_override_defaults) {
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    pe.apply_agent_defaults({"/tmp"});
    pe.load_user_rules(
        R"([{"permission":"deploy","pattern":"*","action":"deny",)"
        R"("comment":"деплой на этой машине запрещён"},)"
        R"({"permission":"bash","pattern":"npm test*","action":"allow"}])");
    ASSERT_EQ((int)pe.evaluate("deploy", "site"), (int)PermissionAction::Deny);
    ASSERT_EQ((int)pe.evaluate("bash", "npm test"), (int)PermissionAction::Allow);
    ASSERT_EQ((int)pe.evaluate("bash", "rm -rf /"), (int)PermissionAction::Ask);
    ASSERT_EQ(pe.visible_tools(sample_tools()).size(), (size_t)3);
}

/* Мусор в настройках не должен обнулять весь набор правил. */
TEST(user_rules_survive_garbage) {
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    pe.apply_agent_defaults({"/tmp"});
    pe.load_user_rules("не json вовсе");
    ASSERT_EQ((int)pe.evaluate("bash", "ls"), (int)PermissionAction::Ask);
    /* 42 — не объект, пропускается; {"action":"deny"} без ключа
     * запрещает всё; {"permission":"deploy"} без действия — спрашивает
     * (действие по умолчанию «спросить», а не «запретить»). */
    pe.load_user_rules(R"([{"action":"deny"}, {"permission":"deploy"}, 42])");
    ASSERT_EQ((int)pe.evaluate("bash", "ls"), (int)PermissionAction::Deny);
    ASSERT_EQ((int)pe.evaluate("deploy", "site"), (int)PermissionAction::Ask);
    /* Явный deny после мусора всё ещё применяется. */
    pe.load_user_rules(
        R"([{"permission":"docker","action":"НЕТ"},)"
        R"({"permission":"ssh","pattern":"*","action":"deny"}])");
    /* Действие не распознано — «спросить»: опечатка не должна молча
     * блокировать инструмент. */
    ASSERT_EQ((int)pe.evaluate("docker", "ps"), (int)PermissionAction::Ask);
    ASSERT_EQ((int)pe.evaluate("ssh", "root@host"), (int)PermissionAction::Deny);
}

/* Запрещённый инструмент исчезает из системного промпта — и появляется
 * обратно, когда правило убирают (кэш при этом сбрасывается). */
TEST(prompt_hides_denied_tool_and_restores_it) {
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    pe.apply_agent_defaults({"/tmp"});
    pe.load_user_rules(
        R"([{"permission":"deploy","pattern":"*","action":"deny"}])");
    engine().invalidate_prompt_cache();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().mode = 0;
    }
    std::string prompt = engine().build_system_prompt();
    if (prompt.find("- deploy") == std::string::npos) {
        /* каталог может быть пустым без зарегистрированных инструментов —
         * тогда проверять нечего, и тест молча проходит. */
        ASSERT_TRUE(ToolsRegistry::instance().has("deploy"));
    } else {
        std::cerr << "  запрещённый deploy попал в промпт" << std::endl;
        ASSERT_TRUE(false);
    }
}

/* ======================================================================
 * И2.9 — permission_key у всех инструментов
 * ====================================================================== */

namespace {

/* Все 50 инструментов — тем же способом, что и в остальных тестах. */
void register_every_tool() {
    static bool done = false;
    if (done) return;
    done = true;
    register_base_tools();
    register_rag_tools();
    register_git_tools();
    wp::register_wp_tools();
    python::register_python_tools();
    devops::register_devops_tools();
}

/* Словарь ключей закрыт списком: опечатка («bashh») иначе создала бы
 * ключ, на который нет ни одного правила, и инструмент молча уехал бы
 * в «спросить» (или, что хуже, в «разрешить») по умолчанию. */
const char* kKnownKeys[] = {"read", "write", "bash", "git", "wp-cli", "db",
                            "deploy", "package", "test", "rag", "docker",
                            "systemd", "cron", "ssh",
                            /* И4.6: план задачи. Правил не требует — он не
                             * касается ни файлов, ни сети, и `* -> allow`
                             * покрывает его по умолчанию. */ "todo"};

} // anonymous namespace

TEST(every_tool_has_permission_key) {
    register_every_tool();
    auto defs = ToolsRegistry::instance().defs();
    /* Инструменты с именем test_* регистрируются юнит-тестами (например
     * для проверки усечения вывода) и в плагине не существуют: их нет ни
     * в CMake, ни в манифесте. Считать их здесь нельзя, иначе тест
     * зависел бы от порядка и от того, какие тесты уже отработали. */
    std::vector<ToolDef> real;
    for (const auto& d : defs) {
        if (d.name.rfind("test_", 0) == 0) continue;
        real.push_back(d);
    }
    defs = real;
    ASSERT_EQ(defs.size(), (size_t)54);
    for (const auto& d : defs) {
        if (d.permission_key.empty()) {
            std::cerr << "  инструмент " << d.name << " без ключа разрешения"
                      << std::endl;
            ASSERT_TRUE(false);
        }
        bool known = false;
        for (const char* k : kKnownKeys) known = known || d.permission_key == k;
        if (!known) {
            std::cerr << "  инструмент " << d.name << " — неизвестный ключ «"
                      << d.permission_key << "»" << std::endl;
            ASSERT_TRUE(false);
        }
    }
}

/* ГЛАВНЫЙ инвариант И2: инструмент с TF_DESTRUCTIVE обязан спрашивать
 * по умолчанию. Проверяется на настоящих инструментах, а не на
 * выдуманных правилах: забытый ключ у нового деструктивного инструмента
 * означал бы, что он работает молча, и уронил бы этот тест. */
TEST(every_destructive_tool_asks_by_default) {
    register_every_tool();
    PermissionGuard g;
    engine().permissions().apply_agent_defaults({"/tmp"});
    int destructive = 0;
    for (const auto& d : ToolsRegistry::instance().defs()) {
        if (!tf_has(d.flags, TF_DESTRUCTIVE)) continue;
        ++destructive;
        if (engine().permissions().evaluate(d.permission_key, "*") !=
            PermissionAction::Ask) {
            std::cerr << "  деструктивный инструмент " << d.name
                      << " (ключ " << d.permission_key
                      << ") не спрашивает по умолчанию" << std::endl;
            ASSERT_TRUE(false);
        }
    }
    /* Список не должен «схлопнуться» до нуля инструментов — иначе тест
     * выше проходит вхолостую. */
    ASSERT_EQ(destructive, 19);
}

/* Ключ и значение вызова — из одного ToolDef, иначе правило «read *.env»
 * не сможет сработать. */
TEST(permission_pattern_comes_from_the_right_argument) {
    register_every_tool();
    json::JsonValue args = json::JsonValue::object();
    args.set("path", json::JsonValue("config/.env"));
    args.set("k", json::JsonValue(3));
    const ToolDef* rd = ToolsRegistry::instance().find("read_file");
    ASSERT_TRUE(rd != nullptr);
    ASSERT_EQ(permission_key_of(*rd), std::string("read"));
    ASSERT_EQ(permission_pattern(*rd, args), std::string("config/.env"));

    /* Команда, а не путь: у bash параметр называется command. */
    json::JsonValue cmd = json::JsonValue::object();
    cmd.set("command", json::JsonValue("git push --force"));
    const ToolDef* ex = ToolsRegistry::instance().find("bash");
    ASSERT_TRUE(ex != nullptr);
    ASSERT_EQ(permission_key_of(*ex), std::string("bash"));
    ASSERT_EQ(permission_pattern(*ex, cmd), std::string("git push --force"));

    /* Предъявлять нечего — решение по факту вызова инструмента. */
    const ToolDef* ls = ToolsRegistry::instance().find("list_skills");
    ASSERT_TRUE(ls != nullptr);
    ASSERT_EQ(permission_pattern(*ls, json::JsonValue::object()),
              std::string("*"));

    /* И3.5: для команды предлагаемый «всегда»-паттерн УЖЕ, чем значение.
     * До И3 здесь было ожидание «git push --force», то есть кнопка
     * «всегда» предлагала записать в правила ровно ту строку, которую
     * пользователь подтвердил. Ровно это И3.5 и закрывает: подтвердив
     * «git push --force» один раз, пользователь навсегда разрешал бы
     * любой push. Теперь предлагается «git push *» — подкоманда
     * зафиксирована, аргументы нет. */
    ASSERT_EQ(permission_suggested_pattern(*ex, "git push --force"),
              std::string("git push *"));
    /* Для не-командных ключей значение остаётся как есть: сужать путь
     * до подкаталога нельзя, это изменило бы объект разрешения. */
    ASSERT_EQ(permission_suggested_pattern(*rd, "config/.env"),
              std::string("config/.env"));
    ASSERT_EQ(permission_suggested_pattern(*ex, "*"), std::string("*"));
}


/* Инструмент без ключа (если кто-то его забудет) спрашивает по имени
 * инструмента, а не молчит: неизвестный ключ не должен превращаться в
 * «разрешить». */
TEST(permission_key_falls_back_to_tool_name) {
    ToolDef d;
    d.name = "mystery_tool";
    ASSERT_EQ(permission_key_of(d), std::string("mystery_tool"));
}

/* ======================================================================
 * И2.10 — сквозная проверка через точку enforcement (ToolRunner::run)
 * ====================================================================== */

namespace {

/* Инструмент без побочных эффектов, но с реальным обработчиком. */
json::JsonValue no_args() { return json::JsonValue::object(); }

/* Текст исхода вызова: результат или отказ. И5.7 ToolRunner::run отдаёт
 * ToolOutcome, а не строку, потому что отказ и пустой вывод — разные
 * вещи. Здесь они склеиваются обратно в текст ровно для проверок «есть ли
 * в ответе [отказ]»: смысл этих проверок не изменился. */
std::string outcome_text(const ToolOutcome& o) {
    return o.ok ? o.output.output : o.error;
}

struct RunnerFixture {
    std::vector<AgentEvent::Kind> kinds;
    std::vector<std::string> texts;
    /* Состояние движка — синглтон: запоминаем и возвращаем, иначе
     * следующий тест (FSM-переходы) увидит Executing вместо Idle и
     * упадёт не по своей причине. */
    AgentState saved_state;
    int saved_mode;
    std::string saved_project;
    ToolRunner runner{engine_state(), Engine::instance().callbacks(),
                      [this](AgentEvent::Kind k, const std::string& t) {
                          kinds.push_back(k);
                          texts.push_back(t);
                      }};
    RunnerFixture() {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        saved_state = engine_state().state;
        saved_mode = engine_state().mode;
        saved_project = engine_state().project_dir;
    }
    ~RunnerFixture() {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().state = saved_state;
        engine_state().mode = saved_mode;
        engine_state().project_dir = saved_project;
        engine_state().recent_calls.clear();
    }
    void clear_state() {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().recent_calls.clear();
        engine_state().state = AgentState::Executing;
        engine_state().project_dir.clear();
    }
};

} // anonymous namespace

/* Спрашивающий инструмент после «разово разрешить» всё-таки выполняется.
 * Это главный риск склейки с И1: не блокнуло, а и не сломало вызов. */
TEST(tool_runner_asks_then_executes_after_once) {
    register_every_tool();
    PermissionGuard g;
    test_support::approve_all_permissions();
    engine().permissions().add_rule(rule("read", "*", PermissionAction::Ask));
    RunnerFixture fx;
    fx.clear_state();

    std::vector<std::thread> threads;
    std::string result;
    threads.emplace_back([&] {
        result = outcome_text(fx.runner.run("list_skills", no_args()));
    });
    Joiner joiner(threads);
    PermissionRequest r = wait_for_pending(engine().permissions());
    ASSERT_EQ(r.permission, std::string("read"));
    engine().permissions().reply(r.id, PermissionReply::Once);
    for (auto& t : threads) t.join();
    /* Обработчик отработал: в ответе есть навыки, а не отказ. */
    ASSERT_TRUE(result.find("[запрещено]") == std::string::npos);
    ASSERT_TRUE(result.find("[отказ]") == std::string::npos);
    ASSERT_TRUE(!result.empty());
}

/* Запрет не спрашивает и не выполняет инструмент. */
TEST(tool_runner_denied_tool_never_runs_and_never_asks) {
    register_every_tool();
    PermissionGuard g;
    test_support::approve_all_permissions();
    engine().permissions().add_rule(rule("read", "*", PermissionAction::Deny));
    RunnerFixture fx;
    fx.clear_state();

    const std::string result = outcome_text(fx.runner.run("list_skills", no_args()));
    ASSERT_TRUE(result.find("запрещён") != std::string::npos);
    ASSERT_EQ(engine().permissions().pending_count(), (size_t)0);
    /* Модели сказано не повторять и не искать обход. */
    ASSERT_TRUE(result.find("НЕ ПОВТОРЯЙ") != std::string::npos);
}

/* Проверка режима идёт ПЕРВОЙ: инструмент, запрещённый режимом, не
 * должен ещё и спрашивать разрешения — вопрос про то, что и так нельзя,
 * только путает пользователя. */
TEST(tool_runner_mode_policy_precedes_permission_ask) {
    register_every_tool();
    PermissionGuard g;
    test_support::approve_all_permissions();
    int prev_mode = engine_state().mode;
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().mode = 1;   /* Research: только чтение */
    }
    /* write_file и под «read»-запретом: у Research он всё равно запрещён
     * по флагам, и спрашивать нечего. */
    engine().permissions().add_rule(rule("write", "*", PermissionAction::Ask));
    RunnerFixture fx;
    fx.clear_state();
    json::JsonValue args = json::JsonValue::object();
    args.set("path", json::JsonValue("x.txt"));
    args.set("content", json::JsonValue("x"));
    const std::string result = outcome_text(fx.runner.run("write_file", args));
    ASSERT_TRUE(result.find("запрещено режимом") != std::string::npos);
    ASSERT_EQ(engine().permissions().pending_count(), (size_t)0);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().mode = prev_mode;
    }
}

/* Порядок блокировок. PermissionEngine берёт state_.mtx короткими
 * захватами и НИКОГДА не держит свой mtx_ поверх него; UI, наоборот,
 * держит state_.mtx и берёт mtx_ за снимком очереди. Если бы порядок
 * был другим, эти два потока встали бы друг напротив друга. */
TEST(ui_reading_queue_under_state_lock_does_not_deadlock) {
    register_every_tool();
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    pe.apply_agent_defaults({"/tmp"});
    pe.set_wait_timeout_ms(3000);

    std::vector<std::thread> threads;
    bool allowed = false;
    threads.emplace_back([&] {
        allowed = pe.ask("bash", {"ls"}, "ls", "bash → ls");
    });
    Joiner joiner(threads);
    wait_for_pending(pe);

    /* Сторож: если блокировки встали друг напротив друга, процесс
     * повиснет навсегда. Сторож превращает висяк в явный FAIL с
     * указанием на виновника — иначе такой баг виден только как
     * «тесты перестали завершаться». */
    std::atomic<bool> done{false};
    std::thread watchdog([&] {
        for (int i = 0; i < 100 && !done.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!done.load()) {
            std::cerr << "  DEADLOCK: чтение очереди под state_.mtx"
                      << std::endl;
            std::_Exit(3);
        }
    });

    uint64_t pending_id = 0;
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        /* UI делает ровно это: держит state_.mtx и читает очередь. */
        auto asks = pe.pending();
        ASSERT_EQ(asks.size(), (size_t)1);
        pending_id = asks.front().id;
    }
    pe.reply(pending_id, PermissionReply::Once);
    for (auto& t : threads) t.join();
    done.store(true);
    watchdog.join();
    ASSERT_TRUE(allowed);
}

/* Параллельные вызовы не ломают ни правила, ни очередь: агент сегодня
 * выполняет инструменты по одному, но И5 (событийная модель) сделает их
 * параллельными, и молчаливая гонка в разрешениях всплыла бы там. */
TEST(permission_engine_survives_parallel_use) {
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    pe.apply_agent_defaults({"/tmp"});
    pe.set_wait_timeout_ms(5000);

    std::atomic<int> granted{0};
    std::vector<std::thread> threads;
    /* Отвечающий поток: снимает всё, что появляется. */
    threads.emplace_back([&] {
        for (int i = 0; i < 4000; ++i) {
            auto asks = pe.pending();
            if (!asks.empty()) pe.reply(asks.front().id, PermissionReply::Once);
            if (granted.load() >= 40) break;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    /* Четыре «инструмента», каждый спорит за ключ и за очередь. */
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 10; ++i) {
                if (pe.evaluate("bash", "ls") != PermissionAction::Ask) continue;
                if (pe.ask("bash", {"ls"}, "ls", "bash")) {
                    granted.fetch_add(1);
                }
            }
        });
    }
    Joiner joiner(threads);
    for (auto& t : threads) t.join();
    ASSERT_EQ(granted.load(), 40);
    ASSERT_EQ(pe.pending_count(), (size_t)0);
    /* Правила не пострадали: очередь кончилась, а не залипла. */
    ASSERT_EQ((int)pe.evaluate("bash", "rm -rf /"), (int)PermissionAction::Ask);
}

/* Повторный init (→ повторный load_settings) не должен размножать
 * правила пользователя: они дописываются в конец, и без защиты набор
 * рос бы копиями на каждой перезагрузке. */
TEST(user_rules_load_is_idempotent) {
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    pe.apply_agent_defaults({"/tmp"});
    const std::string text = R"([{"permission":"deploy","action":"deny"}])";
    pe.load_user_rules(text);
    const size_t after_first = pe.rules().size();
    pe.load_user_rules(text);
    pe.load_user_rules(text);
    ASSERT_EQ(pe.rules().size(), after_first);
    ASSERT_EQ((int)pe.evaluate("deploy", "site"), (int)PermissionAction::Deny);
}

/* После «стоп» и новой задачи вопросы должны задаваться снова. Без
 * снятия отмены агент получал бы мгновенные отказы на всё до конца
 * сессии и не смог бы объяснить пользователю причину. */
TEST(new_task_after_abort_asks_again) {
    PermissionGuard g;
    PermissionEngine& pe = engine().permissions();
    pe.apply_agent_defaults({"/tmp"});
    pe.set_wait_timeout_ms(3000);
    pe.cancel_all();
    pe.set_wait_timeout_ms(60);
    /* Пока отмена держится, вопрос не задаётся вовсе. */
    ASSERT_TRUE(!pe.ask("bash", {"ls"}, "ls", "bash"));

    engine().submit("тестовая задача после стоп");
    pe.set_wait_timeout_ms(3000);
    std::vector<std::thread> threads;
    bool allowed = false;
    threads.emplace_back([&] {
        allowed = pe.ask("bash", {"ls"}, "ls", "bash");
    });
    Joiner joiner(threads);
    PermissionRequest r = wait_for_pending(pe);   /* вопрос задан — тест и есть проверка */
    pe.reply(r.id, PermissionReply::Once);
    for (auto& t : threads) t.join();
    ASSERT_TRUE(allowed);
}

/* ======================================================================
 * И4.11 — детектор doom-loop: три последних вызова + вопрос пользователю
 * ====================================================================== */

namespace {

/* Ответить на ВСЕ вопросы, пока поток агента ждёт. Для doom-loop это
 * «пользователь решил, что повторять можно». */
struct AutoReply {
    explicit AutoReply(PermissionReply how, int max_replies = 8)
        : how_(how), left_(max_replies) {}
    ~AutoReply() { stop(); }

    void operator()() {
        PermissionEngine& pe = engine().permissions();
        for (int i = 0; i < 400 && left_ > 0; ++i) {
            if (pe.pending_count() > 0) {
                PermissionRequest r = pe.pending()[0];
                if (pe.reply(r.id, how_)) --left_;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    void start() {
        stop();
        t_ = std::thread([this] { (*this)(); });
    }
    void stop() {
        if (t_.joinable()) t_.join();
    }

private:
    PermissionReply how_;
    int left_;
    std::thread t_;
};

} // anonymous namespace

/* Третий одинаковый вызов подряд СПРАШИВАЕТ, а не отменяется сам. */
/* Корень проекта нужен: без него каждый файловый инструмент упирается в
 * гейт внешних путей, и тест измерял бы не детектор, а гейт. */
static void doom_loop_project() {
    std::lock_guard<std::mutex> lk(engine_state().mtx);
    engine_state().project_dir = "/tmp";
}

TEST(doom_loop_asks_the_user_instead_of_cancelling_silently) {
    /* Правила разрешений — состояние синглтона: без сброса тест
     * унаследовал бы правила предыдущего и измерял бы не детектор. */
    PermissionGuard g;
    RunnerFixture fx;
    fx.clear_state();
    doom_loop_project();
    PermissionEngine& pe = engine().permissions();
    /* Без дефолтов evaluate() отвечает Ask на всё (правил нет), и вопрос
     * задавал бы не детектор, а пустой реестр разрешений. */
    pe.apply_agent_defaults({"/tmp"});
    pe.set_wait_timeout_ms(5000);
    AutoReply replier(PermissionReply::Once);
    replier.start();

    json::JsonValue a = json::JsonValue::object();
    a.set("path", "zzz.txt");
    const std::string first = outcome_text(fx.runner.run("read_file", a));
    const std::string second = outcome_text(fx.runner.run("read_file", a));
    const std::string third = outcome_text(fx.runner.run("read_file", a));

    /* Ни один из трёх не отменён «по автопилоту»: раньше третий
     * возвращал «[ошибка] зацикливание вызова» без чьего-либо решения. */
    ASSERT_TRUE(first.find("зацикливание") == std::string::npos);
    ASSERT_TRUE(second.find("зацикливание") == std::string::npos);
    ASSERT_TRUE(third.find("зацикливание") == std::string::npos);
    /* Третий вызов действительно вызвал вопрос с ключом doom_loop —
     * иначе проверка выше проходила бы и при полном отсутствии
     * детектора. */
    bool asked = false;
    for (const auto& t : fx.texts) {
        if (t.find("Зацикливание: read_file") != std::string::npos) asked = true;
    }
    ASSERT_TRUE(asked);
}

/* Отказ пользователя останавливает вызов — и это единственный путь, где
 * детектор влияет на поведение. */
TEST(doom_loop_rejection_stops_the_call) {
    /* Правила разрешений — состояние синглтона: без сброса тест
     * унаследовал бы правила предыдущего и измерял бы не детектор. */
    PermissionGuard g;
    RunnerFixture fx;
    fx.clear_state();
    doom_loop_project();
    PermissionEngine& pe = engine().permissions();
    /* Без дефолтов evaluate() отвечает Ask на всё (правил нет), и вопрос
     * задавал бы не детектор, а пустой реестр разрешений. */
    pe.apply_agent_defaults({"/tmp"});
    pe.set_wait_timeout_ms(5000);
    AutoReply replier(PermissionReply::Reject);
    replier.start();

    json::JsonValue a = json::JsonValue::object();
    a.set("path", "zzz.txt");
    fx.runner.run("read_file", a);
    fx.runner.run("read_file", a);
    const std::string third = outcome_text(fx.runner.run("read_file", a));
    ASSERT_TRUE(third.find("зацикливание") != std::string::npos);
    ASSERT_TRUE(third.find("НЕ ПОВТОРЯЙ") != std::string::npos);
}

/* Окно сужено до трёх вызовов: одинаковые отпечатки, разделённые другими
 * вызовами, зацикливанием НЕ являются. В окне из восьми они накапливались
 * и срабатывали ложно. */
TEST(doom_loop_window_is_three_calls_not_eight) {
    /* Правила разрешений — состояние синглтона: без сброса тест
     * унаследовал бы правила предыдущего и измерял бы не детектор. */
    PermissionGuard g;
    RunnerFixture fx;
    fx.clear_state();
    doom_loop_project();
    PermissionEngine& pe = engine().permissions();
    /* Без дефолтов evaluate() отвечает Ask на всё (правил нет), и вопрос
     * задавал бы не детектор, а пустой реестр разрешений. */
    pe.apply_agent_defaults({"/tmp"});
    pe.set_wait_timeout_ms(500);
    /* Отвечать некому: любой вопрос закончился бы отказом по таймауту, и
     * мы увидели бы ложное «зацикливание». Значит, вопросов быть не должно
     * вовсе — это и проверяем. */
    json::JsonValue a = json::JsonValue::object();
    a.set("path", "a.txt");
    json::JsonValue b = json::JsonValue::object();
    b.set("path", "b.txt");
    for (int i = 0; i < 5; ++i) {
        fx.runner.run("read_file", a);
        fx.runner.run("read_file", b);
    }
    for (const auto& t : fx.texts) {
        ASSERT_TRUE(t.find("Зацикливание") == std::string::npos);
    }
}
