// permission_engine.cpp — движок разрешений (И2.3+).

#include "permission_engine.h"
#include "engine.h"
#include "json.h"

#include <algorithm>
#include <chrono>

namespace coder {

PermissionEngine::PermissionEngine() = default;

void PermissionEngine::bind(
        EngineState* state, std::function<void(const std::string&)> push) {
    std::lock_guard<std::mutex> lk(mtx_);
    state_ = state;
    push_ = std::move(push);
}

void PermissionEngine::set_rules(Ruleset r) {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        rules_ = std::move(r);
        /* Набор правил пришёл снаружи целиком — значит, это и есть
         * полный набор, включая дефолты. Иначе повторный load_settings
         * (он зовётся при каждом init) стёр бы ответы «всегда». */
        defaults_installed_ = true;
        cb = on_change_;
    }
    /* Вне лока: обработчик дергает кэш промпта, а тот — снова в
     * core. Внутри mtx_ любой вызов в чужой код — зацепка для дедлока. */
    if (cb) cb();
}

void PermissionEngine::add_rule(Rule r) {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        rules_.add(std::move(r));
        cb = on_change_;
    }
    if (cb) cb();
}

std::string PermissionEngine::rules_dump() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return rules_.dump();
}

PermissionAction PermissionEngine::evaluate(const std::string& permission,
                                            const std::string& pattern) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return rules_.evaluate(permission, pattern);
}

void PermissionEngine::reset() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        rules_.clear();
        defaults_installed_ = false;
        user_rules_loaded_ = false;
        last_user_rules_text_.clear();
        /* Отмена и очередь — тоже состояние сессии: после сброса
         * движок снова готов задавать вопросы, иначе новый запуск после
         * «стоп» получал бы мгновенные отказы без объяснения. */
        cancelled_ = false;
        pending_.clear();
        cb = on_change_;
    }
    if (cb) cb();
    cv_.notify_all();
}

bool PermissionEngine::has_agent_defaults() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return defaults_installed_;
}

void PermissionEngine::set_wait_timeout_ms(int ms) {
    std::lock_guard<std::mutex> lk(mtx_);
    wait_timeout_ms_ = ms > 0 ? ms : 0;
}

int PermissionEngine::wait_timeout_ms() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return wait_timeout_ms_;
}

void PermissionEngine::apply_agent_defaults(
        const std::vector<std::string>& trusted_dirs) {
    /* Идемпотентно: база ставится один раз за сессию. Иначе повторный
     * load_settings (он зовётся на каждом init) стёр бы ответы
     * «всегда», накопленные пользователем, — то есть настройка,
     * которую он сделал час назад, молча исчезла бы. Пересобрать базу
     * можно только через reset() + apply_agent_defaults(). */
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (defaults_installed_) return;
    }

    Ruleset rs;

    /* ПОРЯДОК ПРАВИЛ ЗДЕСЬ НЕ КОСМЕТИКА, А СЕМАНТИКА.
     * Побеждает последнее совпавшее, поэтому «* → allow» обязан быть
     * ПЕРВЫМ: это слабейшее правило, и всё, что описано ниже, его
     * перекрывает. Поставь его в конец — и ни одно исключение из
     * списка не сработает, то есть все опасные инструменты станут
     * доступны молча. */
    rs.add("*", "*", PermissionAction::Allow);

    /* Секреты. Паттерн «*.env*», а не «*.env»: файлы .env.local и
     * .env.production встречаются чаще, чем .env, и оба содержат
     * пароли. Лишний вопрос — не страшно, лишнее чтение секрета — да. */
    rs.add("read", "*.env*", PermissionAction::Ask);

    /* Группы, где действие необратимо или уходит наружу. Каждая
     * строка — отдельная группа инструментов (ключ в permission_key,
     * И2.9), и «всегда» на одну группу не разрешает другую. */
    rs.add("bash", "*", PermissionAction::Ask);
    rs.add("git", "*", PermissionAction::Ask);
    rs.add("wp-cli", "*", PermissionAction::Ask);
    rs.add("db", "*", PermissionAction::Ask);
    rs.add("deploy", "*", PermissionAction::Ask);
    rs.add("systemd", "*", PermissionAction::Ask);
    rs.add("ssh", "*", PermissionAction::Ask);
    rs.add("docker", "*", PermissionAction::Ask);
    rs.add("cron", "*", PermissionAction::Ask);
    rs.add("package", "*", PermissionAction::Ask);
    rs.add("rag", "*", PermissionAction::Ask);
    /* pytest гоняет код проекта — то есть исполняет произвольный код
     * из дерева, которое агент только что правил. */
    rs.add("test", "*", PermissionAction::Ask);

    /* Исключения ПОСЛЕ общих запретов — иначе last-match-wins съедает
     * их, и git status начнёт спрашивать (именно так и вышло в первый
     * вариант этого списка: `git → ask` стоял ниже `git status* →
     * allow` и перекрывал его целиком). */
    rs.add("git", "status*", PermissionAction::Allow);
    rs.add("git", "diff*", PermissionAction::Allow);
    rs.add("git", "log*", PermissionAction::Allow);

    /* Петля инструментов (детектор и сам вопрос — И4.11; здесь
     * объявлен ключ, чтобы к тому моменту правило уже было). */
    rs.add("doom_loop", "*", PermissionAction::Ask);

    /* Выход за пределы проекта. Правило общее, а whitelist ниже его
     * перекрывает для каталогов, куда агенту ходить можно всегда:
     * собственные данные плагина, временный каталог и навыки.
     * Без whitelist плагин спрашивал бы разрешение на каждый заход в
     * /tmp, и вопрос превратился бы в помеху. */
    rs.add("external_directory", "*", PermissionAction::Ask);
    for (const auto& dir : trusted_dirs) {
        if (dir.empty()) continue;
        std::string d = dir;
        while (d.size() > 1 && d.back() == '/') d.pop_back();
        rs.add("external_directory", d, PermissionAction::Allow);
        /* «<dir>/*», а не «<dir>*»: иначе доверенным окажется и
         * каталог, имя которого начинается так же — /tmp-secrets при
         * доверенном /tmp. */
        rs.add("external_directory", d + "/*", PermissionAction::Allow);
    }

    set_rules(std::move(rs));
}

void PermissionEngine::approve_path(const std::string& path) {
    std::string d = path;
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    if (d.empty()) return;
    approve("external_directory", d);
    approve("external_directory", d + "/*");
}

void PermissionEngine::set_on_change(std::function<void()> cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    on_change_ = std::move(cb);
}

/* --- И2.5: ожидание пользователя --- */

bool PermissionEngine::ask(const std::string& permission,
                           const std::vector<std::string>& patterns,
                           const std::string& always_pattern,
                           const std::string& metadata) {
    PermissionRequest req;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (cancelled_) return false;   /* уже отклонено каскадом */
        req.id = ++next_id_;
        req.permission = permission;
        req.patterns = patterns;
        req.suggested = always_pattern;
        req.metadata = metadata;
        pending_.push_back(req);
    }

    /* Мост в состояние движка: агент официально ждёт пользователя.
     * Отдельный короткий захват, mtx_ уже отпущен. Прежнее состояние
     * запоминаем, чтобы вернуть как было: агент мог быть в Planning. */
    AgentState prev = AgentState::Executing;
    if (state_) {
        std::lock_guard<std::mutex> lk(state_->mtx);
        prev = state_->state;
        state_->state = AgentState::WaitingPermission;
    }

    std::string notice = "[permission] " + permission;
    if (!metadata.empty()) notice += ": " + metadata;
    if (push_) push_(notice);

    /* Ожидание. Предикат — только по СВОИМ полям: прерывание задачи
     * приходит как cancel_all() из Engine::request_abort/stop, а не
     * чтением state_ под mtx_. Читать state_->shutting_down, удерживая
     * mtx_, значило бы взять state_.mtx поверх mtx_ — обратный порядок
     * блокировок, то есть дедлок. */
    bool allowed = false;
    {
        std::unique_lock<std::mutex> lk(mtx_);
        auto decided = [&] {
            for (const auto& r : pending_) {
                if (r.id == req.id) return r.decided;
            }
            return true;   /* снят из очереди — отвечать нечего */
        };
        auto done = [&] { return decided() || cancelled_; };

        if (wait_timeout_ms_ > 0) {
            auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(wait_timeout_ms_);
            while (!done()) {
                if (cv_.wait_until(lk, deadline) == std::cv_status::timeout) break;
            }
        } else {
            cv_.wait(lk, done);
        }

        for (auto& r : pending_) {
            if (r.id != req.id) continue;
            allowed = r.allowed;
            break;
        }
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                      [&req](const PermissionRequest& r) {
                                          return r.id == req.id;
                                      }),
                       pending_.end());
    }

    /* Возврат состояния — тоже отдельным коротким захватом. */
    if (state_) {
        std::lock_guard<std::mutex> lk(state_->mtx);
        if (state_->state == AgentState::WaitingPermission) {
            state_->state = prev;
        }
    }
    if (push_) {
        push_(allowed ? "[permission] " + permission + ": разрешено"
                      : "[permission] " + permission + ": отказ");
    }
    return allowed;
}

bool PermissionEngine::reply(uint64_t id, PermissionReply how) {
    std::string key;
    std::string suggested;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        PermissionRequest* target = nullptr;
        for (auto& r : pending_) {
            if (r.id == id) { target = &r; break; }
        }
        if (!target || target->decided) return false;
        target->decided = true;
        target->allowed = (how == PermissionReply::Once ||
                           how == PermissionReply::Always);
        target->always = (how == PermissionReply::Always);
        if (how == PermissionReply::Always) {
            key = target->permission;
            suggested = target->suggested;
        }
        /* И2.6: отказ каскадирует на все прочие ожидающие. Именно это и
         * делает отказ осмысленным: пользователь, закрывший диалог
         * «нет», не должен затем отвечать на те же вопросы по каждому
         * инструменту, который модель успела нарисовать следом. */
        if (how == PermissionReply::Reject) {
            for (auto& r : pending_) {
                if (r.id == id || r.decided) continue;
                r.decided = true;
                r.allowed = false;
            }
        }
    }
    /* Правило пишется ПОСЛЕ отпускания лока: approve() берёт mtx_ сам,
     * а std::mutex нерекурсивный — вызов под локом был бы самостоятельной
     * блокировкой на самом себе. */
    if (how == PermissionReply::Always && !suggested.empty()) {
        approve(key, suggested);
    }
    cv_.notify_all();
    return true;
}

void PermissionEngine::approve(const std::string& permission,
                               const std::string& pattern) {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        rules_.add(permission, pattern.empty() ? "*" : pattern,
                   PermissionAction::Allow);
        /* И2.7: пересканировать ожидающие. Пользователь ответил «всегда»
         * на одну команду, а в очереди уже висит вопрос про такую же —
         * повторный щелчок по «всегда» ради того же самого был бы
         * требованием к пользователю повторить то же решение второй раз.
         *
         * Пересканиваются только запросы того же ключа: правило
         * относится к одному ключу, и вопрос про другой ключ измениться
         * не мог. */
        for (auto& r : pending_) {
            if (r.decided || r.permission != permission) continue;
            bool now_allowed = false;
            for (const auto& p : r.patterns) {
                if (rules_.evaluate(permission, p) == PermissionAction::Allow) {
                    now_allowed = true;
                    break;
                }
            }
            if (now_allowed) {
                r.decided = true;
                r.allowed = true;
            }
        }
        cb = on_change_;
    }
    if (cb) cb();
    cv_.notify_all();
}

std::vector<PermissionRequest> PermissionEngine::pending() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return pending_;
}

size_t PermissionEngine::pending_count() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return pending_.size();
}

std::vector<std::string> PermissionEngine::visible_tools(
        const std::vector<ToolDef>& all) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::string> out;
    out.reserve(all.size());
    for (const auto& def : all) {
        if (rules_.denies_all(permission_key_of(def))) continue;
        out.push_back(def.name);
    }
    return out;
}

void PermissionEngine::load_user_rules(const std::string& json_text) {
    /* Повторная загрузка того же текста ничего не делает: load_settings
     * зовётся на каждом init, а правила дописываются в конец, и без этой
     * проверки набор разросся бы копиями на каждой перезагрузке. */
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (user_rules_loaded_ && json_text == last_user_rules_text_) return;
        last_user_rules_text_ = json_text;
        user_rules_loaded_ = true;
    }

    json::JsonValue root;
    if (!json::JsonValue::parse(json_text, root) || !root.is_array()) return;

    /* Мусор в одной строке не должен лишать пользователя всех правил:
     * каждое правило разбирается отдельно, непонятные пропускаются. */
    std::vector<Rule> parsed;
    for (size_t i = 0; i < root.size(); ++i) {
        const json::JsonValue& item = root.at(i);
        if (!item.is_object()) continue;
        Rule r;
        r.permission = item.has("permission") ? item.get_string("permission") : "*";
        r.pattern = item.has("pattern") ? item.get_string("pattern") : "*";
        r.comment = item.get_string("comment");
        std::string act = item.get_string("action", "ask");
        if (act == "allow" || act == "Allow") {
            r.action = PermissionAction::Allow;
        } else if (act == "deny" || act == "Deny") {
            r.action = PermissionAction::Deny;
        } else {
            /* «ask» и всё незнакомое — спрашивать. Опечатка в действии
             * не должна молча разрешать то, что человек хотел запретить. */
            r.action = PermissionAction::Ask;
        }
        if (r.permission.empty()) continue;
        parsed.push_back(std::move(r));
    }
    if (parsed.empty()) return;

    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& r : parsed) rules_.add(std::move(r));
        cb = on_change_;
    }
    if (cb) cb();
}

std::string PermissionEngine::user_rules_json() const {
    std::lock_guard<std::mutex> lk(mtx_);
    json::JsonValue arr = json::JsonValue::array();
    for (const auto& r : rules_.rules()) {
        json::JsonValue o = json::JsonValue::object();
        o.set("permission", json::JsonValue(r.permission));
        o.set("pattern", json::JsonValue(r.pattern));
        const char* act = "ask";
        if (r.action == PermissionAction::Allow) act = "allow";
        else if (r.action == PermissionAction::Deny) act = "deny";
        o.set("action", json::JsonValue(act));
        if (!r.comment.empty()) o.set("comment", json::JsonValue(r.comment));
        arr.push_back(std::move(o));
    }
    return arr.dump();
}

void PermissionEngine::cancel_all() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        cancelled_ = true;
    }
    cv_.notify_all();
}

void PermissionEngine::clear_cancel() {
    std::lock_guard<std::mutex> lk(mtx_);
    cancelled_ = false;
}

} // namespace coder
