/*
 * test_permission.cpp — И2.1–И2.2: правила разрешений и вилки.
 *
 * Проверяется не «как выглядит список правил», а два свойства, на
 * которых держатся и настройки агентов (И8), и наследование
 * разрешений субагентом (И8.10):
 *   - порядок правил является семантикой (последнее совпадение выигрывает);
 *   - паттерн — вилка, а не регулярное выражение.
 *
 * Тесты эти чистые: ни движка, ни UI, ни потоков. Всё, что связано с
 * ожиданием пользователя, проверяется в test_permission_engine.cpp —
 * иначе тест встал бы колом в ожидании ответа, которого не будет.
 */

#include "test_framework.h"
#include "../core/permission.h"

using namespace coder;

namespace {

Rule rule(const char* key, const char* pattern, PermissionAction action) {
    Rule r;
    r.permission = key;
    r.pattern = pattern;
    r.action = action;
    return r;
}

} // anonymous namespace

/* ======================================================================
 * И2.1 — семантика упорядоченных правил
 * ====================================================================== */

/* Пустой набор правил — это «спросить», а не «разрешить».
 * Обратное поведение — молчаливое разрешение всего, чего не описано, —
 * означало бы, что опечатка в имени ключа снимает защиту целиком. */
TEST(ruleset_without_rules_asks) {
    Ruleset rs;
    ASSERT_TRUE(rs.empty());
    ASSERT_EQ((int)rs.evaluate("bash", "rm -rf /"), (int)PermissionAction::Ask);
    ASSERT_EQ((int)rs.evaluate("deploy", "site.example.com"), (int)PermissionAction::Ask);
}

TEST(ruleset_single_rule_applies) {
    Ruleset rs;
    rs.add(rule("bash", "*", PermissionAction::Ask));
    ASSERT_EQ((int)rs.evaluate("bash", "git status"), (int)PermissionAction::Ask);
    ASSERT_EQ((int)rs.evaluate("read", "a.txt"), (int)PermissionAction::Ask);
}

/* Главное свойство: последнее совпавшее правило выигрывает. Именно ради
 * него правила хранятся вектором, а не «лучшим по приоритету». */
TEST(ruleset_last_match_wins) {
    Ruleset rs;
    rs.add(rule("*", "*", PermissionAction::Allow));
    rs.add(rule("bash", "*", PermissionAction::Deny));
    ASSERT_EQ((int)rs.evaluate("bash", "ls"), (int)PermissionAction::Deny);
    /* read не описан — остаётся общий allow */
    ASSERT_EQ((int)rs.evaluate("read", "a.txt"), (int)PermissionAction::Allow);

    /* Обратный порядок даёт обратный результат: перекрытие идёт
     * дописыванием в конец, а не «важностью». */
    Ruleset reversed;
    reversed.add(rule("bash", "*", PermissionAction::Deny));
    reversed.add(rule("*", "*", PermissionAction::Allow));
    ASSERT_EQ((int)reversed.evaluate("bash", "ls"), (int)PermissionAction::Allow);
}

TEST(ruleset_star_key_covers_all_groups) {
    Ruleset rs;
    rs.add(rule("*", "*", PermissionAction::Allow));
    rs.add(rule("deploy", "*", PermissionAction::Ask));
    /* deploy описан точечно, остальные ключи берут общий allow */
    ASSERT_EQ((int)rs.evaluate("deploy", "site"), (int)PermissionAction::Ask);
    ASSERT_EQ((int)rs.evaluate("bash", "ls"), (int)PermissionAction::Allow);
    /* другое правило для того же ключа перекрывает и его.
     * Паттерн «site.staging*» подобран под И2.1: полная вилка в любой
     * позиции («*.staging») появляется в И2.2 — Wildcard::match. */
    rs.add(rule("deploy", "site.staging*", PermissionAction::Deny));
    ASSERT_EQ((int)rs.evaluate("deploy", "site.staging"), (int)PermissionAction::Deny);
    ASSERT_EQ((int)rs.evaluate("deploy", "site.example.com"), (int)PermissionAction::Ask);
}

TEST(ruleset_mentions_key_separates_explicit_from_catch_all) {
    Ruleset rs;
    rs.add(rule("*", "*", PermissionAction::Allow));
    /* Ключ не описан — действует умолчание уровня агента (И2.4). */
    ASSERT_TRUE(!rs.mentions_key("deploy"));
    rs.add(rule("deploy", "*", PermissionAction::Ask));
    ASSERT_TRUE(rs.mentions_key("deploy"));
    /* mentions_key не путает «описан точечно» с «запрещён целиком». */
    ASSERT_TRUE(!rs.denies_all("deploy"));
    rs.add(rule("deploy", "*", PermissionAction::Deny));
    ASSERT_TRUE(rs.denies_all("deploy"));
    ASSERT_TRUE(!rs.denies_all("bash"));
}

/* Запрет конкретного паттерна инструмент из схемы НЕ убирает: иначе
 * запрет «read *.env» прятал бы read_file целиком, и модель не смогла бы
 * прочитать ни одного файла проекта. denies_all() — только про pattern
 * «*»: это отдельный вопрос, нельзя ли вызывать инструмент вообще. */
TEST(ruleset_specific_deny_does_not_hide_tool) {
    Ruleset rs;
    rs.add(rule("read", "*.env", PermissionAction::Deny));
    ASSERT_TRUE(!rs.denies_all("read"));
}

TEST(ruleset_dump_is_human_readable) {
    Ruleset rs;
    Rule r = rule("bash", "*", PermissionAction::Ask);
    r.comment = "произвольные команды";
    rs.add(r);
    std::string d = rs.dump();
    ASSERT_TRUE(d.find("bash") != std::string::npos);
    ASSERT_TRUE(d.find("спросить") != std::string::npos);
    ASSERT_TRUE(d.find("произвольные команды") != std::string::npos);
}
/* ======================================================================
 * И2.2 — Wildcard::match: `*` и `?`
 * ====================================================================== */

TEST(wildcard_star_matches_any_sequence) {
    ASSERT_TRUE(Wildcard::match("*", ""));
    ASSERT_TRUE(Wildcard::match("*", "anything at all"));
    ASSERT_TRUE(Wildcard::match("*.env", ".env"));
    ASSERT_TRUE(Wildcard::match("*.env", "config/.env"));
    ASSERT_TRUE(Wildcard::match("*.env", "/srv/site/.env"));
    /* «*.env» — суффикс, а не подстрока: «.env.production» под него не
     * подходит. Такие файлы закрывает более широкое правило «*.env*»
     * в дефолтах агента (И2.4) — там сознательно в сторону лишнего
     * вопроса, а не лишнего чтения секрета. */
    ASSERT_TRUE(!Wildcard::match("*.env", "/srv/site/.env.production"));
    ASSERT_TRUE(Wildcard::match("git status*", "git status --porcelain"));
    ASSERT_TRUE(Wildcard::match("*notes*", "/home/user/notes/todo.md"));
    ASSERT_TRUE(!Wildcard::match("*.env", ".environment"));
    ASSERT_TRUE(!Wildcard::match("git status*", "git push --force"));
}

TEST(wildcard_question_mark_matches_single_char) {
    /* `?` — ровно один символ, шаблон в целом ни к чему не «растягивается». */
    ASSERT_TRUE(Wildcard::match("wp_?", "wp_a"));
    ASSERT_TRUE(!Wildcard::match("wp_?", "wp_cli"));
    ASSERT_TRUE(!Wildcard::match("wp_?", "wp_"));
    ASSERT_TRUE(Wildcard::match("a?c", "abc"));
    ASSERT_TRUE(!Wildcard::match("a?c", "ac"));
}

/* Вилка в середине + жадный `*` с откатом. Жадность без отката — это
 * классический баг: «*.env» не нашёл бы «a.b.c.env» откатом, хотя по
 * смыслу `*` в середине строки уже разрешает такое. */
TEST(wildcard_middle_star_backtracks) {
    ASSERT_TRUE(Wildcard::match("*.env", "a.b.c.env"));
    ASSERT_TRUE(Wildcard::match("a*c", "abbbc"));
    ASSERT_TRUE(!Wildcard::match("a*c", "abbbd"));
    ASSERT_TRUE(Wildcard::match("*env", ".env"));
    ASSERT_TRUE(!Wildcard::match("*env", "envelope"));
    ASSERT_TRUE(Wildcard::match("**", "любая строка"));
}

/* Узоры разрешений пишутся людьми в настройках, поэтому «почти совпало»
 * должно быть промахом, а не совпадением. */
TEST(wildcard_is_exact_otherwise) {
    ASSERT_TRUE(Wildcard::match("git status", "git status"));
    ASSERT_TRUE(!Wildcard::match("git status", "git status "));
    ASSERT_TRUE(!Wildcard::match("git status", "GIT STATUS"));
    ASSERT_TRUE(Wildcard::match("", ""));
    ASSERT_TRUE(!Wildcard::match("", "x"));
}

/* Правила, собранные Wildcard, ведут себя как задумано в связке с
 * Ruleset: суффиксный запрет перекрывает общий allow. */
TEST(ruleset_env_rule_deny_overrides_allow) {
    Ruleset rs;
    rs.add(rule("*", "*", PermissionAction::Allow));
    rs.add(rule("read", "*.env", PermissionAction::Deny));
    ASSERT_EQ((int)rs.evaluate("read", "/srv/app/.env"), (int)PermissionAction::Deny);
    ASSERT_EQ((int)rs.evaluate("read", "/srv/app/config.php"), (int)PermissionAction::Allow);
}
