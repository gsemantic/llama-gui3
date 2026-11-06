// permission.cpp — упорядоченные правила разрешений (И2.1).
//
// Здесь только данные и чистая логика «что решили правила». Ожидание
// пользователя (PermissionEngine::ask) живёт в permission_engine.cpp —
// оно блокирует поток и работает с состоянием движка, а этот файл
// обязан оставаться проверяемым без всякого UI.

#include "permission.h"

#include <algorithm>
#include <sstream>

namespace coder {

const char* permission_action_name(PermissionAction a) {
    switch (a) {
        case PermissionAction::Allow: return "разрешить";
        case PermissionAction::Deny:  return "запретить";
        case PermissionAction::Ask:   return "спросить";
    }
    return "спросить";
}

bool Wildcard::match(const std::string& pattern, const std::string& value) {
    /* Две позиции: p — в паттерне, v — в значении. При встрече `*`
     * запоминаем позицию и идём дальше с «пустым» совпадением; если
     * в конце не сошлось — откатываемся к запомненной позиции и
     * пробуем `*` съесть на один символ больше. Именно откат отличает
     * вилку от регулярки: без него «*.env» не нашло бы «a.b.c.env»,
     * хотя по смыслу `*` в середине строки такое разрешает. */
    size_t p = 0, v = 0;
    size_t star_p = std::string::npos;
    size_t star_v = 0;

    while (v < value.size()) {
        if (p < pattern.size() &&
            (pattern[p] == '?' || pattern[p] == value[v])) {
            ++p;
            ++v;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star_p = p++;      /* запомнили `*` и пошли без неё */
            star_v = v;
        } else if (star_p != std::string::npos) {
            /* Не сошлось: откатываемся и даём последней `*` съесть
             * ещё один символ. */
            p = star_p + 1;
            v = ++star_v;
        } else {
            return false;
        }
    }
    /* Хвост паттерна из одних звёздочек ничего не требует от значения. */
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

namespace {

/* Правило применимо к паре (ключ, значение)? Ключ «*» — любая группа. */
bool rule_applies(const Rule& r, const std::string& permission,
                  const std::string& value) {
    if (r.permission != "*" && r.permission != permission) return false;
    return Wildcard::match(r.pattern, value);
}

} // anonymous namespace

void Ruleset::add(Rule r) {
    rules_.push_back(std::move(r));
}

void Ruleset::add(const std::string& permission, const std::string& pattern,
                  PermissionAction action) {
    Rule r;
    r.permission = permission;
    r.pattern = pattern;
    r.action = action;
    rules_.push_back(std::move(r));
}

void Ruleset::clear() { rules_.clear(); }

size_t Ruleset::erase_matching(const std::function<bool(const Rule&)>& pred) {
    const size_t before = rules_.size();
    rules_.erase(std::remove_if(rules_.begin(), rules_.end(), pred),
                 rules_.end());
    return before - rules_.size();
}

bool Ruleset::empty() const { return rules_.empty(); }

size_t Ruleset::size() const { return rules_.size(); }

const std::vector<Rule>& Ruleset::rules() const { return rules_; }

PermissionAction Ruleset::evaluate(const std::string& permission,
                         const std::string& pattern) const {
    bool matched = false;
    return evaluate_matched(permission, pattern, &matched);
}

PermissionAction Ruleset::evaluate_matched(const std::string& permission,
                                            const std::string& pattern,
                                            bool* matched) const {
    /* Идём по всем правилам и запоминаем последнее подходящее — это и
     * есть last match wins. Обрываться на первом совпадении нельзя:
     * тогда «* → allow» в начале списка забил бы все точечные
     * исключения, дописанные позже. */
    PermissionAction result = PermissionAction::Ask;
    bool found = false;
    for (const auto& r : rules_) {
        if (!rule_applies(r, permission, pattern)) continue;
        result = r.action;
        found = true;
    }
    if (matched) *matched = found;
    return result;
}

bool Ruleset::mentions_key(const std::string& permission) const {
    /* Только явное упоминание ключа. Правило «*» НЕ считается: иначе
     * mentions_key() всегда истинно, и нельзя отличить «ключ описан
     * явно» от «подпал под общее правило» — а именно это различие
     * нужно, чтобы поверх правил применялись дефолты уровня агента
     * (И2.4) для ещё не описанных ключей. */
    for (const auto& r : rules_) {
        if (r.permission == permission) return true;
    }
    return false;
}

bool Ruleset::denies_all(const std::string& permission) const {
    for (const auto& r : rules_) {
        if (r.action != PermissionAction::Deny) continue;
        if (r.permission != "*" && r.permission != permission) continue;
        /* Запрет «всё под ключом» — только паттерн «*»: запрет
         * «read *.env» инструмент read_file не убирает, он лишь
         * запрещает читать конкретные файлы. */
        if (r.pattern == "*") return true;
    }
    return false;
}

std::string Ruleset::dump() const {
    std::string out;
    for (const auto& r : rules_) {
        out += r.permission;
        out += " ";
        out += r.pattern.empty() ? "*" : r.pattern;
        out += " → ";
        out += permission_action_name(r.action);
        if (!r.comment.empty()) {
            out += "  (";
            out += r.comment;
            out += ")";
        }
        out += "\n";
    }
    return out;
}

} // namespace coder
