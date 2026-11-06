/*
 * test_agent_registry.cpp — И8.1: реестр агентов.
 *
 * Проверяется не «реестр существует», а четыре контракта, на которых
 * потом стоят И8.2–И8.13 (см. шапку core/agent_registry.h):
 *
 *   1. ИМЯ. Недопустимое имя отвергается С ПРИЧИНОЙ; имя в другом
 *      регистре и с пробелами по краям ведёт к тому же агенту.
 *   2. ПОРЯДОК. Перечисление идёт в порядке регистрации, а не по
 *      алфавиту, и ЗАМЕНА агента место не меняет: этот порядок попадает
 *      в описание инструмента `task` в каждом ходе.
 *   3. ВИДИМОСТЬ. mode решает, кто зовёт (пользователь / `task`), а
 *      hidden прячет из обоих списков, но не из поиска по имени.
 *   4. ЦЕЛОСТНОСТЬ ПОД ГОНКОЙ. Реестр читают UI и worker агента
 *      одновременно, поэтому снимок не может быть наполовину собран.
 *
 * Чего здесь НЕТ и почему это не упрощение: вызывающего. Агентами
 * пользуются И8.5 (встроенные) и И8.7 (инструмент `task`), а
 * сегодня реестр наполняют только тесты. Плата за это названа в
 * отклонении №71: между 8.1 и 8.5 у кода есть структура без
 * пользователя — ровно как у 7.1–7.4 до 7.10.
 */

#include "test_framework.h"
#include "../core/agent_registry.h"
#include "../core/limits.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace coder;

namespace {

/* Определение для теста: обязательное только имя. */
AgentDef def(const std::string& name, AgentMode mode = AgentMode::All,
             bool hidden = false, const std::string& description = "") {
    AgentDef d;
    d.name = name;
    d.mode = mode;
    d.hidden = hidden;
    d.description = description.empty() ? ("описание " + name) : description;
    return d;
}

std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) {
        if (!out.empty()) out += ",";
        out += s;
    }
    return out;
}

/* Список имён из all() — в том порядке, в каком их отдаёт реестр. */
std::vector<std::string> names_in_order(const AgentRegistry& reg) {
    std::vector<std::string> names;
    for (const auto& p : reg.all()) names.push_back(p->name);
    return names;
}

} // namespace

/* ======================================================================
 * 1. Имя
 * ====================================================================== */

TEST(agent_registry_add_and_find) {
    AgentRegistry reg;
    ASSERT_TRUE(reg.add(def("wp_build")));
    ASSERT_EQ(reg.size(), (size_t)1);
    ASSERT_TRUE(reg.has("wp_build"));

    const auto found = reg.find("wp_build");
    ASSERT_TRUE(found != nullptr);
    ASSERT_EQ(found->name, std::string("wp_build"));
    ASSERT_EQ(std::string(agent_mode_name(found->mode)), std::string("all"));

    /* Несуществующий — nullptr, а не «агент с пустым именем»: иначе
     * вызывающий (И8.7) отличил бы опечатку модели от агента с
     * пустым описанием только по содержимому указателя. */
    ASSERT_TRUE(reg.find("wp_nope") == nullptr);
    ASSERT_FALSE(reg.has("wp_nope"));
}

TEST(agent_registry_rejects_invalid_names) {
    AgentRegistry reg;
    struct Bad {
        const char* name;
        const char* why;   /* nullptr — причина не проверяется */
    };
    const std::vector<Bad> bad = {
        {"", nullptr},
        {"   ", nullptr},
        {"wp build", nullptr},
        {"wp*build", "*"},
        {"wp.build", "."},
        {"wp/build", "/"},
        {"wp:build", ":"},
        {"построить", nullptr},          /* не-ASCII отвергается (шапка) */
    };
    for (const Bad& b : bad) {
        std::string why;
        const bool ok = reg.add(def(b.name), &why);
        if (ok) {
            std::cerr << "  имя «" << b.name << "» принято реестром, а должно"
                      << " быть отвергнуто" << std::endl;
        }
        ASSERT_FALSE(ok);
        /* Причина обязана быть непустой: отвергнутое имя без объяснения
         * заставит искать ошибку в парсере frontmatter (И8.2), где её
         * нет. */
        ASSERT_TRUE(!why.empty());
        if (b.why) {
            const bool mentions = why.find(b.why) != std::string::npos;
            if (!mentions) {
                std::cerr << "  причина отказа «" << why
                          << "» не называет виновный символ «" << b.why
                          << "»" << std::endl;
            }
            ASSERT_TRUE(mentions);
        }
    }
    /* Ни один отказ не должен оставить след в реестре. */
    ASSERT_EQ(reg.size(), (size_t)0);
    ASSERT_TRUE(join(reg.primary_names()).empty());
    ASSERT_TRUE(join(reg.subagent_names()).empty());
}

TEST(agent_registry_rejects_too_long_name) {
    AgentRegistry reg;
    const std::string ok(limits::kMaxAgentNameLen, 'a');
    ASSERT_TRUE(reg.add(def(ok)));

    /* Ровно на один символ длиннее предела — уже отказ. Граница
     * вычисляется из константы, а не подгоняется: число в проверке
     * обязано быть тем же, что в коде, иначе проверка врала бы о коде
     * (урок И7.9 про подогнанные числа). */
    const std::string too_long(limits::kMaxAgentNameLen + 1, 'b');
    std::string why;
    const bool accepted = reg.add(def(too_long), &why);
    if (accepted) {
        std::cerr << "  имя длиной " << too_long.size() << " принято, предел "
                  << limits::kMaxAgentNameLen << std::endl;
    }
    ASSERT_FALSE(accepted);
    ASSERT_TRUE(!why.empty());
    ASSERT_TRUE(reg.find(too_long) == nullptr);
    ASSERT_EQ(reg.size(), (size_t)1);
}

TEST(agent_registry_normalizes_name_case_and_spaces) {
    AgentRegistry reg;
    ASSERT_TRUE(reg.add(def("  WP_Build \t")));

    /* Зарегистрированное имя каноническое: его и показывают в списках,
     * и кладут в описание инструмента `task` (И8.13). */
    ASSERT_EQ(join(reg.primary_names()), std::string("wp_build"));

    /* Имя приходит и от модели, и из имени файла, и оба пишут его как
     * получилось. Отказ по регистру означал бы: агент есть, он в
     * списке — и «нет такого агента». */
    ASSERT_TRUE(reg.find("wp_build") != nullptr);
    ASSERT_TRUE(reg.find("WP_BUILD") != nullptr);
    ASSERT_TRUE(reg.find("Wp_Build") != nullptr);
    ASSERT_TRUE(reg.find("\twp_build\n") != nullptr);
    ASSERT_TRUE(reg.has("WP_BUILD"));
    ASSERT_EQ(reg.size(), (size_t)1);
}

/* ======================================================================
 * 2. Порядок и замена
 * ====================================================================== */

TEST(agent_registry_lists_in_registration_order) {
    AgentRegistry reg;
    ASSERT_TRUE(reg.add(def("zzz")));
    ASSERT_TRUE(reg.add(def("aaa")));
    ASSERT_TRUE(reg.add(def("mmm")));

    /* Порядок регистрации, а НЕ алфавитный: иначе встроенные агенты
     * (wp_*) оказались бы после пользовательских, а список в промпте
     * зависел бы от того, какие файлы лежат в каталоге. */
    ASSERT_EQ(join(names_in_order(reg)), std::string("zzz,aaa,mmm"));
}

TEST(agent_registry_replaces_same_name_in_place) {
    AgentRegistry reg;
    ASSERT_TRUE(reg.add(def("wp_build", AgentMode::All, false, "встроенный")));
    ASSERT_TRUE(reg.add(def("wp_plan", AgentMode::All, false, "встроенный план")));
    ASSERT_TRUE(reg.add(def("my_agent", AgentMode::All, false, "из конфига")));

    /* Переопределение — НЕ ошибка: пользовательский конфиг перекрывает
     * встроенного (так и в порте). */
    std::string why;
    const bool ok = reg.add(def("WP_Plan", AgentMode::All, false, "перекрыт"), &why);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(why.empty());

    /* Размер не вырос: второго агента с тем же именем не появилось. */
    ASSERT_EQ(reg.size(), (size_t)3);
    const auto plan = reg.find("wp_plan");
    ASSERT_TRUE(plan != nullptr);
    ASSERT_EQ(plan->description, std::string("перекрыт"));

    /* Место не сдвинулось — порядок попадает в описание `task` в
     * каждом ходе, и перестановка из-за одного файла конфига ломала бы
     * сравнение ходов. */
    ASSERT_EQ(join(names_in_order(reg)), std::string("wp_build,wp_plan,my_agent"));
}

/* ======================================================================
 * 3. Видимость
 * ====================================================================== */

TEST(agent_registry_modes_split_who_may_call) {
    AgentRegistry reg;
    ASSERT_TRUE(reg.add(def("both", AgentMode::All)));
    ASSERT_TRUE(reg.add(def("user_only", AgentMode::Primary)));
    ASSERT_TRUE(reg.add(def("task_only", AgentMode::Subagent)));

    ASSERT_EQ(join(reg.primary_names()), std::string("both,user_only"));
    ASSERT_EQ(join(reg.subagent_names()), std::string("both,task_only"));

    /* Строки режимов уходят в конфиг и в разбор frontmatter (И8.2),
     * поэтому закреплены здесь, а не «где-то в порте». */
    ASSERT_EQ(std::string(agent_mode_name(AgentMode::Primary)),
              std::string("primary"));
    ASSERT_EQ(std::string(agent_mode_name(AgentMode::Subagent)),
              std::string("subagent"));
    ASSERT_EQ(std::string(agent_mode_name(AgentMode::All)), std::string("all"));
}

TEST(agent_registry_hidden_is_listed_nowhere_but_findable) {
    AgentRegistry reg;
    ASSERT_TRUE(reg.add(def("visible")));
    ASSERT_TRUE(reg.add(def("secret", AgentMode::All, /*hidden=*/true)));

    /* hidden — это «не показывать», а не «выключить»: явный вызов по
     * имени (slash-команда, И11) обязан работать, иначе агент,
     * спрятанный от списка, был бы ещё и недоступным. */
    ASSERT_EQ(join(reg.primary_names()), std::string("visible"));
    ASSERT_EQ(join(reg.subagent_names()), std::string("visible"));
    ASSERT_TRUE(reg.find("secret") != nullptr);
    /* В all() скрытый есть: all() — это «что зарегистрировано», а не
     * «что показывается». */
    ASSERT_EQ(join(names_in_order(reg)), std::string("visible,secret"));
}

/* ======================================================================
 * 4. Снятие
 * ====================================================================== */

TEST(agent_registry_remove_keeps_the_rest_intact) {
    AgentRegistry reg;
    ASSERT_TRUE(reg.add(def("a")));
    ASSERT_TRUE(reg.add(def("b", AgentMode::All, false, "первый вариант")));
    ASSERT_TRUE(reg.add(def("c")));

    ASSERT_TRUE(reg.remove("A"));           /* имя нормализуется и здесь */
    ASSERT_FALSE(reg.remove("a"));          /* второй раз — уже нечего */
    ASSERT_FALSE(reg.remove("nope"));
    ASSERT_EQ(reg.size(), (size_t)2);
    ASSERT_TRUE(reg.find("a") == nullptr);

    /* Удаление сдвигает индексы оставшихся. Если этого не учесть,
     * следующая ЗАМЕНА по имени «b» попала бы в чужую позицию — тихая
     * порча, которую не увидит ни один поиск: имя «b» нашлось бы
     * прямо там, где надо. */
    ASSERT_TRUE(reg.add(def("b", AgentMode::All, false, "второй вариант")));
    const auto b = reg.find("b");
    ASSERT_TRUE(b != nullptr);
    ASSERT_EQ(b->description, std::string("второй вариант"));
    ASSERT_EQ(join(names_in_order(reg)), std::string("b,c"));
    ASSERT_EQ(join(reg.primary_names()), std::string("b,c"));
}

TEST(agent_registry_clear_empties_lookup_too) {
    AgentRegistry reg;
    ASSERT_TRUE(reg.add(def("a")));
    ASSERT_TRUE(reg.add(def("b")));

    reg.clear();

    ASSERT_EQ(reg.size(), (size_t)0);
    ASSERT_TRUE(reg.all().empty());
    ASSERT_TRUE(join(reg.primary_names()).empty());
    ASSERT_TRUE(join(reg.subagent_names()).empty());
    /* Имя, снятого агента не находится: остаточная запись в индексе
     * вернула бы агента, которого в реестре нет. */
    ASSERT_TRUE(reg.find("a") == nullptr);

    /* И то же имя снова можно занять (перечитывание конфигурации). */
    ASSERT_TRUE(reg.add(def("a", AgentMode::All, false, "заново")));
    ASSERT_EQ(join(names_in_order(reg)), std::string("a"));
    const auto a = reg.find("a");
    ASSERT_TRUE(a != nullptr);
    ASSERT_EQ(a->description, std::string("заново"));
}

/* ======================================================================
 * 5. Экземпляры и гонка
 * ====================================================================== */

TEST(agent_registry_instances_do_not_share_state) {
    AgentRegistry a;
    AgentRegistry b;
    ASSERT_TRUE(a.add(def("only_in_a")));

    /* Общий синглтон протекал бы между тестами, а состояние агентов
     * — из тех, что положено восстанавливать (правило 5
     * SESSION_START.md). Тесты обязаны иметь свои экземпляры, поэтому
     * конструктор публичный и instance() — отдельная точка входа. */
    ASSERT_FALSE(b.has("only_in_a"));
    ASSERT_EQ(b.size(), (size_t)0);

    ASSERT_TRUE(&AgentRegistry::instance() == &AgentRegistry::instance());
}

TEST(agent_registry_survives_concurrent_replace_and_read) {
    AgentRegistry reg;
    ASSERT_TRUE(reg.add(def("hot")));
    ASSERT_TRUE(reg.add(def("cold")));

    /* Реестр читают UI-поток и worker агента одновременно (D-6 —
     * считать под локом нельзя, значит наружу уходит снимок). Проверка
     * не «просто не падает»: каждый снимок обязан быть ЦЕЛЫМ — снимок,
     * собранный наполовину, дал бы агенту без описания или со сдвинутым
     * именем, и это тихо уехало бы в промпт. */
    const size_t kIterations = 20000;
    std::atomic<bool> stop{false};
    std::atomic<size_t> torn{0};

    std::vector<std::thread> readers;
    for (int i = 0; i < 3; ++i) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                for (const auto& p : reg.all()) {
                    if (p->name.empty() || p->description.empty()) {
                        torn.fetch_add(1);
                    }
                }
                if (reg.find("hot") == nullptr) torn.fetch_add(1);
                if (reg.find("cold") == nullptr) torn.fetch_add(1);
            }
        });
    }

    for (size_t i = 0; i < kIterations; ++i) {
        ASSERT_TRUE(reg.add(def("hot", AgentMode::All, false,
                                std::string("вариант ") + std::to_string(i))));
    }
    stop.store(true);
    for (auto& t : readers) t.join();

    ASSERT_EQ(torn.load(), (size_t)0);
    ASSERT_EQ(reg.size(), (size_t)2);
    /* Определение, отданное ДО последней замены, не должно измениться:
     * на этом стоит неизменяемость, на которую опирается И8.4
     * (предвыровненный Ruleset). */
    const auto hot = reg.find("hot");
    ASSERT_TRUE(hot != nullptr);
    ASSERT_EQ(hot->description, std::string("вариант ") +
                                    std::to_string(kIterations - 1));
}
