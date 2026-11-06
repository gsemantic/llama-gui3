/*
 * harness_profile.cpp — разбор и применение профилей harness (И9.7).
 *
 * Шапка файла (core/harness_profile.h) объясняет, что профиль умеет и
 * что не умеет. Здесь — только разбор и две функции, переводящие профиль
 * в правила: по ключам инструментов и по списку команд. Обе получают
 * свойства, которые меняют (реестр, политику) ПАРАМЕТРОМ — см. о том же
 * в шапке: синглтон здесь означал бы, что проверка не может снять то,
 * что навесила предыдущая.
 */

#include "harness_profile.h"

#include "command_policy.h"
#include "json.h"
#include "text_edit.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace coder {
namespace harness {

namespace {

/* Ключи, которые профиль читает. Что НЕ в списке, попадает в
 * unknown_keys: молча выбросить поле значило бы не заметить опечатку
 * в `tools_policy`, а опечатка в применённом поле молча ничего не
 * делает. */
const std::set<std::string>& known_keys() {
    static const std::set<std::string> keys = {
        "model", "temperature", "max_tokens", "tools_policy", "timeout_ms",
        "allowed_commands", "allowed_extensions", "rag_enabled",
        "description"};
    return keys;
}

/* Обрезать пробелы по краям. У полей из файла это единственная
 * «нормализация»: `allowed_commands` и `tools_policy` приходят от
 * человека, и пробел после запятой не должен становиться именем
 * команды, которого нет в allowlist. */
std::string trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' ||
                     s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

/* Имя в кавычках. Названо НЕ quoted: ADL на std::string находит
 * std::quoted из <iomanip>, и объявление с тем же именем молча
 * проигрывало бы ему — компилятор сказал бы «не найден operator+»
 * на месте, где кавычки и должны были быть. */
std::string q(const std::string& s) { return "\"" + s + "\""; }

/* Проверка «это список строк». why — с названием поля и позицией:
 * «allowed_commands: элемент 2 — не строка» полезно, а «плохой список»
 * заставило бы читать файл целиком. */
bool string_list(const json::JsonValue& v, const char* field,
                 std::vector<std::string>* out, std::string* why) {
    if (!v.is_array()) {
        if (why) *why = std::string(field) + ": ожидался список, а пришло " +
                        v.type_name();
        return false;
    }
    for (size_t i = 0; i < v.size(); ++i) {
        const json::JsonValue& item = v.at(i);
        if (!item.is_string()) {
            if (why) {
                *why = std::string(field) + ": элемент " +
                       std::to_string(i + 1) + " — не строка (" +
                       item.type_name() + ")";
            }
            return false;
        }
        const std::string s = trim(item.as_string());
        if (s.empty()) {
            if (why) {
                *why = std::string(field) + ": элемент " +
                       std::to_string(i + 1) + " пуст";
            }
            return false;
        }
        out->push_back(s);
    }
    return true;
}

} // namespace

bool parse_profile(const std::string& text, Profile* out, std::string* why) {
    if (why) why->clear();
    json::JsonValue root;
    std::string jerr;
    if (!json::JsonValue::parse(text, root, &jerr)) {
        if (why) *why = std::string("файл не разобран как JSON: ") + jerr;
        return false;
    }
    if (!root.is_object()) {
        if (why) {
            *why = std::string("ожидался объект JSON, а пришло ") +
                   root.type_name();
        }
        return false;
    }

    Profile p;

    /* Неизвестные ключи — не отказ. Профили писались под harness, и
     * часть полей (`rag_enabled`, `model`) ядро знает, но применять не
     * может; отвергать файл из-за них значило бы сделать поставленные
     * профили непригодными. Опечатку показываем, а не проглатываем. */
    for (const std::string& k : root.keys()) {
        if (known_keys().count(k) == 0) p.unknown_keys.push_back(k);
    }

    if (root.has("description")) {
        const json::JsonValue& v = root.get("description");
        if (!v.is_string()) {
            if (why) {
                *why = std::string("description: ожидалась строка, а пришло ") +
                       std::string(v.type_name());
            }
            return false;
        }
        p.description = trim(v.as_string());
    }

    if (root.has("model")) {
        const json::JsonValue& v = root.get("model");
        if (!v.is_string()) {
            if (why) {
                *why = std::string("model: ожидалась строка, а пришло ") +
                       std::string(v.type_name());
            }
            return false;
        }
        p.model_hint = trim(v.as_string());
    }

    /* temperature: границы проверяются, хотя поле не применяется.
     * Причина — не строгость ради строгости: значение вне [0; 2] в
     * файле почти всегда опечатка, а И11.14 возьмёт его как есть и
     * отдаст хосту. Отвергнуть сейчас дешевле, чем ловить странный ответ
     * провайдера через две итерации. */
    if (root.has("temperature")) {
        const json::JsonValue& v = root.get("temperature");
        if (!v.is_number()) {
            if (why) {
                *why = std::string("temperature: ожидалось число, а пришло ") +
                       std::string(v.type_name());
            }
            return false;
        }
        const double t = v.as_double();
        if (!(t >= 0.0 && t <= 2.0)) {
            if (why) {
                *why = std::string("temperature: значение вне диапазона [0; 2] — ") +
                       std::to_string(t);
            }
            return false;
        }
        p.has_temperature = true;
        p.temperature = t;
    }

    /* max_tokens и timeout_ms — ЦЕЛЫЕ и положительные. Дробь или ноль
     * отвергаются, а не округляются: округлённый лимит ответов молча
     * обрезал бы ответ модели, а обнулённый таймаут отключал бы ожидание
     * вовсе. */
    const auto read_positive_int = [&](const char* field, bool* has,
                                       int* out_val) -> bool {
        const json::JsonValue& v = root.get(field);
        if (!v.is_int()) {
            if (why) {
                *why = std::string(field) + ": ожидалось целое число, а пришло " +
                       std::string(v.type_name());
            }
            return false;
        }
        const long long n = v.as_int();
        if (n <= 0 ||
            n > static_cast<long long>(std::numeric_limits<int>::max())) {
            if (why) *why = std::string(field) + ": значение вне диапазона 1..2^31-1";
            return false;
        }
        *has = true;
        *out_val = static_cast<int>(n);
        return true;
    };
    if (root.has("max_tokens")) {
        if (!read_positive_int("max_tokens", &p.has_max_tokens, &p.max_tokens)) {
            return false;
        }
    }
    if (root.has("timeout_ms")) {
        if (!read_positive_int("timeout_ms", &p.has_timeout_ms, &p.timeout_ms)) {
            return false;
        }
    }

    if (root.has("tools_policy")) {
        const json::JsonValue& v = root.get("tools_policy");
        if (!v.is_string()) {
            if (why) {
                *why = std::string("tools_policy: ожидалась строка, а пришло ") +
                       std::string(v.type_name());
            }
            return false;
        }
        p.tools_policy = trim(v.as_string());
        /* Неизвестное значение — ОТКАЗ, а не «как standard». Политика
         * инструментов — это запреты, и опечатка в названии, прочитанная
         * как «ничего не запрещать», — самый дорогой вид молчания: файл
         * secure_audit с tools_policy «strictt» выглядел бы защищённым и
         * ничего бы не защищал. */
        if (!valid_tools_policy(p.tools_policy)) {
            if (why) {
                std::string list;
                for (const std::string& n : tools_policy_names()) {
                    if (!list.empty()) list += ", ";
                    list += n;
                }
                *why = std::string("tools_policy: ") + q(p.tools_policy) +
                       " — не известное значение (допустимы: " + list + ")";
            }
            return false;
        }
    }

    if (root.has("allowed_commands") &&
        !string_list(root.get("allowed_commands"), "allowed_commands",
                     &p.allowed_commands, why)) {
        return false;
    }
    if (root.has("allowed_extensions") &&
        !string_list(root.get("allowed_extensions"), "allowed_extensions",
                     &p.allowed_extensions, why)) {
        return false;
    }
    if (root.has("rag_enabled")) {
        const json::JsonValue& v = root.get("rag_enabled");
        if (!v.is_bool()) {
            if (why) {
                *why = std::string("rag_enabled: ожидалось true/false, а пришло ") +
                       std::string(v.type_name());
            }
            return false;
        }
        p.rag_enabled = v.as_bool();
    }

    *out = std::move(p);
    return true;
}

bool valid_profile_name(const std::string& name, std::string* why) {
    if (name.empty()) {
        if (why) *why = "имя профиля пустое";
        return false;
    }
    /* Имя подставляется в путь: точка-разделитель, слеш и обратный слеш
     * в нём означали бы либо выход из каталога, либо имя, которое
     * невозможно набрать в настройке. Всё остальное разрешено —
     * ограничение здесь то же, что у имён агентов (И8.1): не
     * запрещать то, что человек вправе назвать. */
    for (char c : name) {
        const bool bad = c == '/' || c == '\\' || c == '.' || c == '*' ||
                         c == '?' || c == '"' || c == '\'' || c == ' ' ||
                         c == '\t' || c == '\n' || c == '\r' ||
                         static_cast<unsigned char>(c) < 0x20;
        if (!bad) continue;
        if (why) {
            *why = std::string("имя профиля ") + q(name) +
                   " недопустимо: символ " +
                   q(std::string(1, c)) + " нельзя";
        }
        return false;
    }
    return true;
}

std::vector<std::string> profile_names(const std::string& dir) {
    std::vector<std::string> names;
    if (dir.empty()) return names;
    std::error_code ec;
    /* Каталог проверяется ДО iterator'а, и обход идёт одним итератором
     * с одним end: directory_iterator в условии цикла затирает ту самую
     * error_code, которой же обход заканчивается (журнал И9.1–9.6, п. 5). */
    if (!fs::is_directory(dir, ec) || ec) return names;
    fs::directory_iterator it(dir, ec);
    if (ec) return names;
    const fs::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        const fs::path p = it->path();
        if (p.extension() != ".json") continue;
        const std::string stem = p.stem().string();
        std::string why;
        if (!valid_profile_name(stem, &why)) continue;
        names.push_back(stem);
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool load_profile(const std::string& dir, const std::string& name, Profile* out,
                  std::string* why) {
    if (why) why->clear();
    std::string name_why;
    if (!valid_profile_name(name, &name_why)) {
        if (why) *why = name_why;
        return false;
    }
    if (dir.empty()) {
        if (why) {
            *why = "каталог профилей не задан (настройка wp_coder.profiles_dir "
                   "пуста и каталог плагина неизвестен)";
        }
        return false;
    }
    const fs::path path = fs::path(dir) / (name + ".json");
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) {
        if (why) {
            std::string list;
            for (const std::string& n : profile_names(dir)) {
                if (!list.empty()) list += ", ";
                list += n;
            }
            *why = std::string("профиль ") + q(name) +
                   " не найден: " + path.string() +
                   (list.empty() ? " (в каталоге нет ни одного профиля)"
                                 : " (доступны: " + list + ")");
        }
        return false;
    }

    /* Через text_edit.h: форму файла (BOM, CRLF) читает ровно один
     * модуль плагина, и свой разбор байтов здесь означал бы, что
     * правка формата дойдёт когда-нибудь до одной из двух реализаций. */
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (why) *why = std::string("профиль не открылся: ") + path.string();
        return false;
    }
    const std::string raw((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    TextFile tf = split_text(raw);
    std::string body = join_text(tf);

    Profile p;
    if (!parse_profile(body, &p, why)) {
        if (why) *why = path.string() + ": " + *why;
        return false;
    }
    p.name = name;
    p.source_path = path.string();
    *out = std::move(p);
    return true;
}

const std::vector<std::string>& tools_policy_names() {
    /* Порядок — как в описаниях профилей, и он же попадает в сообщение
     * об отказе: человеку показывают варианты в том виде, в каком они
     * перечислены в файлах. */
    static const std::vector<std::string> names = {"standard", "restricted",
                                                   "strict", "verbose"};
    return names;
}

bool valid_tools_policy(const std::string& policy) {
    const std::vector<std::string>& names = tools_policy_names();
    return std::find(names.begin(), names.end(), policy) != names.end();
}

const std::vector<std::string>& strict_allowed_keys() {
    /* Один ключ, и это единственный набор, заданный руками. Полнота
     * проверяется тестом на ТОТАЛЬНОСТЬ (denied_keys покрывает все ключи
     * реестра), поэтому новый инструмент с новым ключом заставит решить:
     * он попадает в этот список или остаётся запрещённым. Молчаливый
     * третий вариант (забыть и про новый ключ) невозможен — проверка
     * падает. */
    static const std::vector<std::string> keys = {"read"};
    return keys;
}

std::vector<std::string> denied_keys(const std::string& policy,
                                     const std::vector<ToolDef>& all,
                                     std::string* why) {
    std::vector<std::string> out;
    if (why) why->clear();
    if (!valid_tools_policy(policy)) {
        if (why) {
            std::string list;
            for (const std::string& n : tools_policy_names()) {
                if (!list.empty()) list += ", ";
                list += n;
            }
            *why = std::string("неизвестная политика инструментов ") + q(policy) +
                   " (допустимы: " + list + ")";
        }
        return out;
    }
    /* standard и verbose ничего не сужают: это режимы «работаем как
     * обычно», а не запреты. У verbose нет иного смысла — журналирование
     * профилем не настраивается, — и принимать его молча было бы обещанием
     * подробного лога, которого не будет (см. шапку). */
    if (policy == "standard" || policy == "verbose") return out;

    /* Ключ → ВСЕ ли его инструменты только читают.
     *
     * Именно «все», и это решение, а не оговорка: единица разрешения —
     * КЛЮЧ (core/permission.h, И2), и правило «ключ: * → запретить» не
     * умеет закрыть один инструмент из группы. Ключ `git` содержит и
     * git_status (только читает), и git_commit (пишет), поэтому
     * «только чтение» на уровне ключа означает «весь git закрыт» — иначе
     * restricted назывался бы «только чтение», а коммитил бы. Обратный
     * вариант (оставить ключ, если хоть один инструмент читает) оставил бы
     * ровно такую дыру у каждого смешанного ключа, а «запретить по
     * большинству» был бы ещё и неустойчив к порядку регистрации.
     *
     * Ключ без TF_READ_ONLY ни у одного инструмента закрывается, и
     * инструмент без флагов (TF_UNCLASSIFIED, И1.3) — тоже: запрет по
     * незнанию есть fail-closed, как дефолт Ask в И2.3. */
    std::map<std::string, bool> key_all_read_only;
    for (const ToolDef& def : all) {
        const std::string key = permission_key_of(def);
        if (key.empty()) continue;
        auto it = key_all_read_only.find(key);
        if (it == key_all_read_only.end()) {
            key_all_read_only[key] = tf_has(def.flags, TF_READ_ONLY);
        } else if (!tf_has(def.flags, TF_READ_ONLY)) {
            it->second = false;
        }
    }

    const std::vector<std::string>& keep = strict_allowed_keys();
    for (const auto& kv : key_all_read_only) {
        if (policy == "restricted") {
            if (!kv.second) out.push_back(kv.first);
            continue;
        }
        /* strict: остаётся только чтение — плана тоже нет. */
        if (std::find(keep.begin(), keep.end(), kv.first) == keep.end()) {
            out.push_back(kv.first);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool apply_allowed_commands(CommandPolicy& policy,
                            const std::vector<std::string>& cmds, std::string* why) {
    if (why) why->clear();
    if (cmds.empty()) return true;   /* поля может не быть вовсе */

    /* Сузить до пересечения: команда, которой нет в allowlist по
     * умолчанию, профилем НЕ открывается. Иначе файл в каталоге плагина
     * становился бы способом расширить границу (И3), а профиль —
     * инструментом обхода политики команд. */
    std::set<std::string> wanted;
    for (const std::string& c : cmds) {
        const std::string name = trim(c);
        if (name.empty()) {
            if (why) *why = "allowed_commands: пустое имя команды";
            return false;
        }
        wanted.insert(name);
    }

    /* Сравнение идёт по ИМЕНИ, как в binary_allowed: там совпадение идёт
     * по всем вариантам (php8.1 → php), и иначе команда, разрешённая
     * профилем, отказала бы по той же причине, по которой её не пускает
     * allowlist по умолчанию. Поэтому имя из профиля сперва приводится к
     * тому же виду, что и хранится в allowlist, — иначе «php8.1» в
     * профиле сузил бы список до нуля. */
    std::set<std::string> keep;
    for (const std::string& name : wanted) {
        if (!policy.binary_allowed(name)) continue;
        ParsedCommand probe;
        probe.binary = name;
        for (const std::string& v : probe.variants()) keep.insert(v);
    }

    const std::set<std::string> current = policy.allowed_binaries();
    for (const std::string& binary : current) {
        if (keep.count(binary) > 0) continue;
        policy.deny_binary(binary);
    }
    return true;
}

} // namespace harness
} // namespace coder
