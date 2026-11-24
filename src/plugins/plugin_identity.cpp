/*
 * plugin_identity.cpp — имя плагина как идентификатор (R0.3).
 *
 * Обоснование и границы — в plugin_identity.h. Здесь только код.
 *
 * ПОЧЕМУ ФАЙЛ, А НЕ plugin_manager.cpp. Та же причина, что у
 * plugin_rag_gate.cpp: функция объявлена в заголовке и обязана иметь
 * внешнюю линковку, а в plugin_manager.cpp всё внутри анонимного
 * namespace. Проверяется тестом на десятках имён без плагинов и без
 * диска.
 */

#include "plugins/plugin_identity.h"

#include <cstdint>

namespace llama_gui {
namespace plugin {

namespace {

inline bool is_lower_alpha(char c) { return c >= 'a' && c <= 'z'; }
inline bool is_digit(char c) { return c >= '0' && c <= '9'; }

/* Допустимый символ внутри имени (после первого). */
inline bool is_name_char(char c) {
    return is_lower_alpha(c) || is_digit(c) || c == '_' || c == '-';
}

/* Короткий детерминированный хэш строки (FNV-1a, 32 бита), 8 hex-цифр.
 *
 * Зачем хэш. Санитизация теряет информацию: `Плагин` и `Модуль` оба
 * дают `plugin`, `WP Coder` и `wp coder` оба дают `wp_coder`. Если имя
 * после санитизации отличается от объявленного, к нему добавляется
 * суффикс `-<hex>`: два разных плагина не получат одно имя (иначе
 * второй молча затёр бы первый в меню, настройках и RAG-скоупе), и при
 * этом имя остаётся читаемым. Валидное имя хэша НЕ получает — `wp_coder`
 * остаётся `wp_coder`.
 *
 * FNV-1a выбран потому, что это десять строк без зависимостей, и его
 * результат стабилен между платформами и компиляторами (в отличие от
 * std::hash, который в libstdc++ и libc++ различается). Стабильность
 * важна: имя попадает в путь на диске и в ключ настроек. */
std::string short_hash(const std::string& s) {
    uint32_t h = 2166136261u;  // FNV offset basis
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;        // FNV prime
    }
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(8);
    for (int shift = 28; shift >= 0; shift -= 4) {
        out.push_back(hex[(h >> shift) & 0xF]);
    }
    return out;
}

} // namespace

bool plugin_name_is_valid(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    // Первая буква — строчная.
    if (!is_lower_alpha(name[0])) return false;
    for (size_t i = 1; i < name.size(); ++i) {
        if (!is_name_char(name[i])) return false;
    }
    return true;
}

std::string plugin_name_sanitize(const std::string& name) {
    std::string out;
    out.reserve(name.size());

    for (char c : name) {
        char mapped;
        if (is_lower_alpha(c) || is_digit(c)) {
            mapped = c;
        } else if (c == '_' || c == '-') {
            mapped = c;
        } else if (c >= 'A' && c <= 'Z') {
            // Заглавные → строчные: `WP Coder` и `wp coder` не должны
            // расходиться (на Windows-ФС они и так неразличимы).
            mapped = static_cast<char>(c - 'A' + 'a');
        } else {
            // Всё прочее (пробел, точка, двоеточие, слеш, не-ASCII) —
            // разделитель. Не-ASCII (русское имя плагина) превращается в
            // `_`; различимость разных имён обеспечивает хэш-суффикс ниже.
            mapped = '_';
        }

        // Схлопывание повторов `_` подряд: `wp  coder` → `wp_coder`, а не
        // `wp__coder`. Край не должен начинаться с `_` — это отсекается
        // ниже, но схлопывание экономит и объясняет, почему `a__b` и
        // `a_b` совпадают: это осознанная склейка, а не потеря.
        if (mapped == '_' && (!out.empty() && out.back() == '_')) continue;

        out.push_back(mapped);
    }

    // Имя обязано начинаться с буквы. Срезаем ведущие `_`/тире, но НЕ
    // цифры: цифра — это данные, а не мусор (`Плагин2` ≠ `Плагин3`).
    size_t first_alpha = 0;
    while (first_alpha < out.size() && !is_lower_alpha(out[first_alpha]) &&
           !is_digit(out[first_alpha])) {
        ++first_alpha;
    }
    if (first_alpha > 0) out.erase(0, first_alpha);

    // Хвостовые `_` — тоже мусор: `wp coder ` → `wp_coder` (пробел уже
    // стал `_` и схлопнулся), и без обрезки имя бы кончлось на `_`.
    while (!out.empty() && out.back() == '_') out.pop_back();

    if (out.empty()) out = "plugin";

    // Начинается с цифры — добавляем букву, иначе имя невалидно по схеме.
    if (is_digit(out[0])) out.insert(out.begin(), 'p');

    // База не длиннее 64: дальше к ней добавится суффикс, и обрезать
    // придётся с его учётом. Без этой обрезки имя из 100 символов
    // возвращалось длиной 69.
    if (out.size() > 64) {
        out.resize(64);
        while (!out.empty() && out.back() == '_') out.pop_back();
    }

    // Хэш-суффикс, если имя изменилось. Без него `Плагин` и `Модуль` оба
    // стали бы `plugin`, и второй плагин молча затёр бы первый. Суффикс
    // ставится после обрезки, поэтому итог может быть короче 64.
    if (out != name) {
        const std::string suffix = "-" + short_hash(name);
        if (out.size() + suffix.size() > 64) {
            out.resize(64 - suffix.size());
            while (!out.empty() && out.back() == '_') out.pop_back();
        }
        out += suffix;
    }

    return out;
}

} // namespace plugin
} // namespace llama_gui