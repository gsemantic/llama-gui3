// test_plugin_identity.cpp — R0.3: имя плагина как идентификатор.
//
// До R0.3 имя из ll_plugin_info() проверялось только на непустоту. Схема
// plugin.schema.json требует `^[a-z][a-z0-9_-]*$`, но кодом схема не
// применяется: .so мог назвать себя `../../etc` — и это имя уходило в
// меню, `__plugin_sep__`, имена окон и (с R1) в путь RAG-скоупа.
//
// Проверяется:
//   - plugin_name_is_valid: что считается валидным идентификатором;
//   - plugin_name_sanitize: что делает с невалидным (и что не теряет
//     смысл: `WP Coder` → `wp_coder`, а не `plugin`);
//   - идемпотентность: sanitize(sanitize(x)) == sanitize(x);
//   - границы: 64 символа, пустое, не-ASCII, `..`, слэши, двоеточия.
//
// Тест не грузит плагины и не читает манифесты: plugin_identity.cpp —
// чистые строковые функции (та же причина, что у gbnf_from_schema).

#include "../../include/plugins/plugin_identity.h"
#include "../test_framework.h"

#include <string>

using namespace llama_gui::plugin;

namespace {

// Проверка «имя валидно» с внятным сообщением: на какой строке и какое
// имя подвело. Без этого падение на таблице из 30 имён не указывает
// виновника.
void assert_valid(const std::string& name) {
    if (!plugin_name_is_valid(name)) {
        std::ostringstream oss;
        oss << "expected valid: '" << name << "'";
        throw std::runtime_error(oss.str());
    }
}

void assert_invalid(const std::string& name) {
    if (plugin_name_is_valid(name)) {
        std::ostringstream oss;
        oss << "expected invalid: '" << name << "'";
        throw std::runtime_error(oss.str());
    }
}

void assert_sanitized(const std::string& input, const std::string& expected) {
    const std::string got = plugin_name_sanitize(input);
    if (got != expected) {
        std::ostringstream oss;
        oss << "sanitize('" << input << "') = '" << got
            << "', expected '" << expected << "'";
        throw std::runtime_error(oss.str());
    }
    // Побочное, но важное: результат обязан быть валидным, иначе
    // sanitize врёт — он обещает вернуть имя, пригодное как ключ.
    if (!plugin_name_is_valid(got)) {
        std::ostringstream oss;
        oss << "sanitize('" << input << "') produced invalid '" << got << "'";
        throw std::runtime_error(oss.str());
    }
}

} // namespace

// ============================================================================
// plugin_name_is_valid
// ============================================================================

void test_valid_names() {
    assert_valid("a");
    assert_valid("wp_coder");
    assert_valid("hello_plugin");
    assert_valid("rag-agent");
    assert_valid("a1");
    assert_valid("plugin123");
    assert_valid("x");
}

void test_invalid_names_empty() {
    assert_invalid("");
}

void test_invalid_names_first_char() {
    assert_invalid("1plugin");    // цифра первой
    assert_invalid("_plugin");    // подчёркивание первой
    assert_invalid("-plugin");    // дефис первой
    assert_invalid(".plugin");    // точка первой
    assert_invalid("Плагин");     // не-ASCII первой
}

void test_invalid_names_forbidden_chars() {
    assert_invalid("wp coder");   // пробел
    assert_invalid("wp.coder");   // точка
    assert_invalid("../etc");     // точки и слэш
    assert_invalid("wp/coder");   // слэш
    assert_invalid("wp:coder");   // двоеточие
    assert_invalid("wp\\coder");  // обратный слэш
    assert_invalid("wp@corder");  // собака
}

void test_invalid_names_uppercase() {
    // Заглавные запрещены: на Windows-ФС `Foo` и `foo` неразличимы.
    assert_invalid("WpCoder");
    assert_invalid("WP_CODER");
}

void test_invalid_names_too_long() {
    const std::string exact_64(64, 'a');
    assert_valid(exact_64);

    const std::string over_65(65, 'a');
    assert_invalid(over_65);
}

void test_invalid_names_unicode_digits() {
    // Арабские цифры — не '0'..'9', значит невалидны.
    assert_invalid("plugin\u0661");  // "plugin١"
}

// ============================================================================
// plugin_name_sanitize
// ============================================================================

void test_sanitize_keeps_valid_names_untouched() {
    assert_sanitized("wp_coder", "wp_coder");
    assert_sanitized("a", "a");
    assert_sanitized("rag-agent", "rag-agent");
}

void test_sanitize_lowercases() {
    // Заглавные → строчные. Имя изменилось, значит появляется хэш-суффикс;
    // проверяем, что база правильная и результат валиден.
    const std::string a = plugin_name_sanitize("WP_CODER");
    const std::string b = plugin_name_sanitize("WpCoder");
    TEST_ASSERT(a.rfind("wp_coder-", 0) == 0);
    TEST_ASSERT(b.rfind("wpcoder-", 0) == 0);
    assert_valid(a);
    assert_valid(b);
}

void test_sanitize_replaces_forbidden_chars() {
    // Запрещённые символы → `_`, плюс хэш-суффикс (имя изменилось).
    const char* in[] = {"wp coder", "wp.coder", "wp/coder", "wp:coder"};
    for (const char* n : in) {
        const std::string got = plugin_name_sanitize(n);
        TEST_ASSERT(got.rfind("wp_coder-", 0) == 0);
        assert_valid(got);
    }
}

void test_sanitize_collapses_repeats() {
    // Схлопывание `_` меняет имя, значит появляется суффикс. Это
    // осознанно: `wp__coder` и `wp_coder` — разные объявленные имена, и
    // без суффикса они стали бы одним.
    const std::string collapsed = plugin_name_sanitize("wp__coder");
    TEST_ASSERT(collapsed.rfind("wp_coder-", 0) == 0);
    assert_valid(collapsed);

    for (const char* n : {"wp  coder", "wp..coder"}) {
        const std::string got = plugin_name_sanitize(n);
        TEST_ASSERT(got.rfind("wp_coder-", 0) == 0);
        assert_valid(got);
    }

    // `wp-_-coder` валидно и не меняется — суффикса нет.
    assert_sanitized("wp-_-coder", "wp-_-coder");
}

void test_sanitize_strips_leading_nonalphas() {
    // Ведущие не-буквы срезаются; имя изменилось → суффикс.
    const char* in[] = {"_plugin", "-plugin", "..plugin"};
    for (const char* n : in) {
        const std::string got = plugin_name_sanitize(n);
        TEST_ASSERT(got.rfind("plugin-", 0) == 0);
        assert_valid(got);
    }

    // Цифра сохранена как данные, получает префикс `p` и суффикс.
    const std::string g = plugin_name_sanitize("1_wp_coder");
    TEST_ASSERT(g.rfind("p1_wp_coder-", 0) == 0);
    assert_valid(g);
}

void test_sanitize_keeps_leading_digits_as_data() {
    // Цифра — данные. `Плагин2` и `Плагин3` обязаны различаться, иначе
    // два плагина схлопнутся в одно имя. Транслитерации нет (ограничение
    // R0.3), различает их хэш-суффикс.
    const std::string p2 = plugin_name_sanitize("Плагин2");
    const std::string p3 = plugin_name_sanitize("Плагин3");
    TEST_ASSERT(p2 != p3);
    assert_valid(p2);
    assert_valid(p3);

    // 123plugin → p123plugin-<hash>
    const std::string g = plugin_name_sanitize("123plugin");
    TEST_ASSERT(g.rfind("p123plugin-", 0) == 0);
    assert_valid(g);
}

void test_sanitize_non_ascii_names_are_distinguishable() {
    // Два русских имени не должны давать одно: иначе второй плагин
    // «затрёт» первый в меню, настройках и RAG-скоупе.
    TEST_ASSERT(plugin_name_sanitize("Плагин") != plugin_name_sanitize("Модуль"));
}

void test_sanitize_strips_trailing_underscores() {
    // Хвостовые `_` срезаются, затем добавляется суффикс.
    // Хвост обрезан. У `wp coder ` в середине пробел → `_`, поэтому база
    // `wp_coder`; у `wp.` и `wp_` база просто `wp`.
    TEST_ASSERT(plugin_name_sanitize("wp coder ").rfind("wp_coder-", 0) == 0);
    TEST_ASSERT(plugin_name_sanitize("wp.").rfind("wp-", 0) == 0);
    TEST_ASSERT(plugin_name_sanitize("wp_").rfind("wp-", 0) == 0);
    for (const char* n : {"wp coder ", "wp.", "wp_"}) {
        assert_valid(plugin_name_sanitize(n));
    }
}

void test_sanitize_traversal_attempts() {
    // Главный враждебный случай: имя пытается выйти из каталога.
    // sanitize обязан дать безопасный идентификатор, а не сохранить `..`.
    const std::string etc = plugin_name_sanitize("../../etc/passwd");
    TEST_ASSERT(etc.rfind("etc_passwd-", 0) == 0);
    assert_valid(etc);

    const std::string wp = plugin_name_sanitize("wp/../coder");
    TEST_ASSERT(wp.rfind("wp_coder-", 0) == 0);
    assert_valid(wp);

    // `..` и `../` вырезаются полностью → дефолт + суффикс.
    const std::string d1 = plugin_name_sanitize("..");
    const std::string d2 = plugin_name_sanitize("../");
    TEST_ASSERT(d1 != d2);   // разные исходные имена → разные суффиксы
    assert_valid(d1);
    assert_valid(d2);
    TEST_ASSERT(d1.rfind("plugin-", 0) == 0);
    TEST_ASSERT(d2.rfind("plugin-", 0) == 0);
}

void test_sanitize_empty_and_punctuation() {
    // Всё вырезано → дефолт `plugin` + суффикс (имя изменилось).
    const char* in[] = {"", "!!!", "...", "   "};
    for (const char* n : in) {
        const std::string got = plugin_name_sanitize(n);
        TEST_ASSERT(got.rfind("plugin-", 0) == 0);
        assert_valid(got);
    }
    // Пустое и «!!!» — разные исходные имена → разные результаты.
    TEST_ASSERT(plugin_name_sanitize("") != plugin_name_sanitize("!!!"));
}

void test_sanitize_non_ascii() {
    // Не-ASCII → `_` (транслитерации нет, это ограничение R0.3).
    // Результат валиден и снабжён хэш-суффиксом.
    const std::string p = plugin_name_sanitize("Плагин");
    TEST_ASSERT(p.rfind("plugin-", 0) == 0);
    assert_valid(p);
}

void test_sanitize_length_limit() {
    const std::string long_name(100, 'a');
    const std::string got = plugin_name_sanitize(long_name);
    // Обрезка до 64 с учётом суффикса: имя длинное, значит суффикс есть,
    // но общая длина не превышает 64.
    TEST_ASSERT(got.size() <= 64u);
    assert_valid(got);
    TEST_ASSERT(got.back() != '_');   // не кончается на подчёркивание
}

void test_sanitize_idempotent() {
    // Ключевое свойство: повторная санитизация ничего не меняет. Иначе
    // имя, прошедшее через два потребителя (меню и RAG-скоуп),
    // разошлось бы само с собой.
    const char* names[] = {
        "wp_coder", "WP Coder", "wp coder", "../etc/passwd", "",
        "!!!", "Плагин", "a", "wp..coder", "123plugin", "wp-_-coder",
    };
    for (const char* n : names) {
        const std::string once = plugin_name_sanitize(n);
        const std::string twice = plugin_name_sanitize(once);
        if (once != twice) {
            std::ostringstream oss;
            oss << "not idempotent for '" << n << "': '" << once
                << "' -> '" << twice << "'";
            throw std::runtime_error(oss.str());
        }
    }
}

void test_sanitize_all_results_valid() {
    // sanitize обещает вернуть имя, пригодное как ключ/часть пути.
    // Проверяем на наборе, где раньше были дыры.
    const char* names[] = {
        "", " ", ".", "..", "/", "\\", ":", "1", "_", "-", "A",
        "wp coder", "../x", "\x01\x02", "Плагин", "plugin\x7f",
    };
    for (const char* n : names) {
        assert_valid(plugin_name_sanitize(n));
    }
}

int main() {
    REGISTER_TEST(test_valid_names);
    REGISTER_TEST(test_invalid_names_empty);
    REGISTER_TEST(test_invalid_names_first_char);
    REGISTER_TEST(test_invalid_names_forbidden_chars);
    REGISTER_TEST(test_invalid_names_uppercase);
    REGISTER_TEST(test_invalid_names_too_long);
    REGISTER_TEST(test_invalid_names_unicode_digits);

    REGISTER_TEST(test_sanitize_keeps_valid_names_untouched);
    REGISTER_TEST(test_sanitize_lowercases);
    REGISTER_TEST(test_sanitize_replaces_forbidden_chars);
    REGISTER_TEST(test_sanitize_collapses_repeats);
    REGISTER_TEST(test_sanitize_strips_leading_nonalphas);
    REGISTER_TEST(test_sanitize_keeps_leading_digits_as_data);
    REGISTER_TEST(test_sanitize_non_ascii_names_are_distinguishable);
    REGISTER_TEST(test_sanitize_strips_trailing_underscores);
    REGISTER_TEST(test_sanitize_traversal_attempts);
    REGISTER_TEST(test_sanitize_empty_and_punctuation);
    REGISTER_TEST(test_sanitize_non_ascii);
    REGISTER_TEST(test_sanitize_length_limit);
    REGISTER_TEST(test_sanitize_idempotent);
    REGISTER_TEST(test_sanitize_all_results_valid);

    return test::TestRunner::instance().run();
}