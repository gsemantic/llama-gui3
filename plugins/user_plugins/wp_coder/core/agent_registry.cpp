#include "agent_registry.h"
#include "limits.h"
#include "tool.h"
#include "tools_registry.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <utility>

namespace fs = std::filesystem;

namespace coder {

/* Порядок режимов в этом enum — не данные, а порядок объявления: он
 * используется только в двух местах обоих, где перебор идёт по явному
 * switch. Сортировать по нему нельзя (тот же запрет, что на
 * «упорядочивание» внутри Ruleset — core/permission.h). */
const char* agent_mode_name(AgentMode mode) {
    switch (mode) {
        case AgentMode::Primary:  return "primary";
        case AgentMode::Subagent: return "subagent";
        case AgentMode::All:      return "all";
    }
    return "all";
}

namespace {

bool is_space(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

bool is_name_char(char c) {
    /* Только ASCII: кириллица в имени агента выглядела бы
     * естественно (имя автора), но ломала бы вызов инструмента `task`
     * — модель и JSON-протокол пришлось бы заставлять передавать
     * не-ASCII строкой, а это ровно тот класс, который в И2.2 уже
     * запретили для шаблонов. Отвергается, а не молча транслитерится:
     * молчаливый транслитеринг дал бы два разных имени для одного
     * агента, и второй был бы не тем, который написал пользователь. */
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
}

} // namespace

bool normalize_agent_name(const std::string& raw, std::string* out) {
    size_t b = 0;
    size_t e = raw.size();
    while (b < e && is_space(raw[b])) ++b;
    while (e > b && is_space(raw[e - 1])) --e;
    std::string name = raw.substr(b, e - b);
    /* Только ASCII-регистр: tolower() из <locale> по умолчанию в
     * UTF-8 ничего не делает, а именно это и нужно — не-ASCII имя всё
     * равно будет отвергнуто проверкой, и менять его регистр нечего. */
    for (char& c : name) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (name.empty()) return false;
    if (out) *out = name;
    return true;
}

bool valid_agent_name(const std::string& name, std::string* why) {
    auto reject = [why](const std::string& reason) {
        if (why) *why = reason;
        return false;
    };
    if (name.empty()) {
        return reject("имя агента пустое");
    }
    if (name.size() > limits::kMaxAgentNameLen) {
        return reject("имя агента '" + name + "' длиннее " +
                      std::to_string(limits::kMaxAgentNameLen) + " символов (" +
                      std::to_string(name.size()) + ")");
    }
    for (size_t i = 0; i < name.size(); ++i) {
        if (is_name_char(name[i])) continue;
        /* Позиция — в БАЙТАХ, и это сказано прямо: имя может содержать
         * не-ASCII (оно будет отвергнуто следующей же строкой), и
         * «символ 4» в таком имени указало бы не туда. */
        return reject("имя агента '" + name + "': недопустимый символ '" +
                      std::string(1, name[i]) + "' на позиции " +
                      std::to_string(i + 1) +
                      " (допустимы a-z, 0-9, «_» и «-»)");
    }
    return true;
}

AgentRegistry& AgentRegistry::instance() {
    static AgentRegistry reg;
    return reg;
}

bool AgentRegistry::add(AgentDef def, std::string* error) {
    std::string name;
    if (!normalize_agent_name(def.name, &name) || !valid_agent_name(name, error)) {
        if (error && error->empty()) {
            *error = "имя агента '" + def.name + "' пустое после срезания пробелов";
        }
        return false;
    }
    def.name = name;
    if (error) error->clear();

    auto held = std::make_shared<AgentDef>(std::move(def));
    std::lock_guard<std::mutex> lk(mtx_);
    const auto it = index_.find(name);
    if (it == index_.end()) {
        index_[name] = order_.size();
        order_.push_back(held);
    } else {
        /* Замена на месте: место в порядке — часть контракта (шапка,
         * п. 2). Меняется только определение, ранее выданные
         * shared_ptr остаются в силе и указывают на прежнего агента —
         * это и есть требуемая неизменяемость (п. 3). */
        order_[it->second] = held;
    }
    return true;
}

std::shared_ptr<const AgentDef> AgentRegistry::find(const std::string& name) const {
    std::string key;
    if (!normalize_agent_name(name, &key)) return nullptr;
    std::lock_guard<std::mutex> lk(mtx_);
    const auto it = index_.find(key);
    if (it == index_.end()) return nullptr;
    return order_[it->second];
}

bool AgentRegistry::has(const std::string& name) const {
    return find(name) != nullptr;
}

std::vector<std::shared_ptr<const AgentDef>> AgentRegistry::snapshot_locked() const {
    std::vector<std::shared_ptr<const AgentDef>> out;
    out.reserve(order_.size());
    for (const auto& p : order_) out.push_back(p);
    return out;
}

std::vector<std::shared_ptr<const AgentDef>> AgentRegistry::all() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return snapshot_locked();
}

std::vector<std::string> AgentRegistry::primary_names() const {
    std::vector<std::string> names;
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& p : order_) {
        if (p->hidden) continue;
        if (p->mode == AgentMode::Subagent) continue;
        names.push_back(p->name);
    }
    return names;
}

std::vector<std::string> AgentRegistry::subagent_names() const {
    std::vector<std::string> names;
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& p : order_) {
        if (p->hidden) continue;
        if (p->mode == AgentMode::Primary) continue;
        names.push_back(p->name);
    }
    return names;
}

bool AgentRegistry::remove(const std::string& name) {
    std::string key;
    if (!normalize_agent_name(name, &key)) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    const auto it = index_.find(key);
    if (it == index_.end()) return false;
    /* Порядок оставшихся сохраняется: сдвиг индексов после удалённой
     * позиции обязателен, иначе следующая замена обновила бы чужой
     * агент — молчаливая порча, которую не увидит ни один тест на
     * поиск. */
    const size_t pos = it->second;
    order_.erase(order_.begin() + static_cast<long>(pos));
    index_.erase(it);
    for (auto& kv : index_) {
        if (kv.second > pos) --kv.second;
    }
    return true;
}

void AgentRegistry::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    order_.clear();
    index_.clear();
}

size_t AgentRegistry::size() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return order_.size();
}

/* ======================================================================
 * И8.2: разбор `.wpcode/agent/*.md`
 * ======================================================================
 *
 * Разбор frontmatter сделан здесь, а не натянут на существующий
 * json_utils.h (D-2): там только извлекатель полей, без вложенности и
 * списков, а у агента есть `tools:` и `permission:` с вложенными
 * картами. Свой разбор — это и есть формат агента; общий YAML-движок
 * был бы зависимостью, которую запрещено заводить.
 */

namespace {

/* Узел разобранного YAML-подмножества. Глубина карт — произвольная
 * (нужна для `permission: {edit: {"*.env": deny}}`), списки — только
 * скалярные: список объектов в описании агента не встречается, и
 * принимать его молча было бы хуже, чем отвергнуть. */
struct YamlNode {
    enum class Kind { Scalar, List, Map };
    Kind kind = Kind::Scalar;
    std::string scalar;
    std::vector<std::string> list;
    std::vector<std::pair<std::string, YamlNode>> map;

    const YamlNode* find(const std::string& key) const {
        for (const auto& kv : map) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
    bool has(const std::string& key) const { return find(key) != nullptr; }
};

struct SrcLine {
    size_t indent = 0;
    std::string text;   /* без отступа и без \r */
    size_t number = 0;  /* номер строки в файле, с единицы */
};

std::string trim_ws(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && is_space(s[b])) ++b;
    while (e > b && is_space(s[e - 1])) --e;
    return s.substr(b, e - b);
}

/* Снять обрамляющие кавычки. Экранирование НЕ разворачивается: в
 * описании агента его нет, а разворачивание «на всякий случай» превратило
 * бы Windows-путь из `C:\\tmp` в `C:\tmp` молча и тихо. */
std::string unquote(const std::string& v) {
    if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') ||
                          (v.front() == '\'' && v.back() == '\''))) {
        return v.substr(1, v.size() - 2);
    }
    return v;
}

/* Склейка списка в строку — для options. Разделитель «, », а не «\n»:
 * значение попадает в текст ошибки и в параметры запроса, где перевод
 * строки читался бы как конец сообщения. */
std::string join_comma(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) {
        if (!out.empty()) out += ", ";
        out += s;
    }
    return out;
}

std::vector<std::string> split_commas(const std::string& v) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : v) {
        if (c == ',') { out.push_back(trim_ws(cur)); cur.clear(); continue; }
        cur += c;
    }
    const std::string last = trim_ws(cur);
    if (!last.empty()) out.push_back(last);
    return out;
}

bool parse_bool(const std::string& v, bool* out) {
    std::string s;
    for (char c : v) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "true" || s == "yes" || s == "on" || s == "1") { *out = true; return true; }
    if (s == "false" || s == "no" || s == "off" || s == "0") { *out = false; return true; }
    return false;
}

bool parse_double(const std::string& v, double* out) {
    std::string s = trim_ws(v);
    if (s.empty()) return false;
    try {
        size_t used = 0;
        const double d = std::stod(s, &used);
        /* std::stod берёт префикс: из «0.2abc» вышел бы 0.2, и опечатка
         * стала бы числом. Хвост — не число. */
        if (trim_ws(s.substr(used)).size() > 0) return false;
        /* nan/inf разбираются std::stod, но агент с такой температурой
         * не имеет смысла, и провайдер либо отвергнет, либо сделает
         * что-то своё. */
        if (!std::isfinite(d)) return false;
        *out = d;
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_int(const std::string& v, int* out) {
    double d = 0.0;
    if (!parse_double(v, &d)) return false;
    if (d != std::floor(d)) return false;
    if (d < -2147483648.0 || d > 2147483647.0) return false;
    *out = static_cast<int>(d);
    return true;
}

/* `allow | ask | deny` — строки из порта. */
bool parse_action(const std::string& v, PermissionAction* out) {
    std::string s;
    for (char c : v) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "allow") { *out = PermissionAction::Allow; return true; }
    if (s == "ask")   { *out = PermissionAction::Ask; return true; }
    if (s == "deny")  { *out = PermissionAction::Deny; return true; }
    return false;
}

class Parser {
public:
    Parser(std::vector<AgentLoadDiag>* diags, std::string path)
        : diags_(diags), path_(std::move(path)) {}

    void diag(size_t line, const std::string& message) {
        if (!diags_) return;
        AgentLoadDiag d;
        d.path = path_;
        d.message = "строка " + std::to_string(line) + ": " + message;
        diags_->push_back(d);
    }

    /* Блок, начинающийся с позиции i и отступа indent. */
    YamlNode parse_block(const std::vector<SrcLine>& ls, size_t& i, size_t indent) {
        YamlNode node;
        if (i >= ls.size() || ls[i].indent < indent) return node;
        if (ls[i].text == "-" || ls[i].text.rfind("- ", 0) == 0) {
            node.kind = YamlNode::Kind::List;
            while (i < ls.size() && ls[i].indent == indent &&
                   (ls[i].text == "-" || ls[i].text.rfind("- ", 0) == 0)) {
                const size_t no = ls[i].number;
                const std::string item = ls[i].text.size() > 1
                                         ? trim_ws(ls[i].text.substr(2)) : "";
                ++i;
                if (item.empty() && i < ls.size() && ls[i].indent > indent) {
                    diag(no, "вложенный элемент списка не поддерживается");
                    skip_deeper(ls, i, indent);
                    continue;
                }
                node.list.push_back(unquote(item));
            }
            return node;
        }
        node.kind = YamlNode::Kind::Map;
        while (i < ls.size() && ls[i].indent == indent) {
            const size_t no = ls[i].number;
            const std::string& t = ls[i].text;
            const size_t colon = t.find(':');
            if (colon == std::string::npos) {
                diag(no, "ожидалась строка «ключ: значение», а это «" + t + "»");
                ++i;
                continue;
            }
            const std::string key = unquote(trim_ws(t.substr(0, colon)));
            const std::string rest = trim_ws(t.substr(colon + 1));
            ++i;
            if (key.empty()) {
                diag(no, "пустой ключ");
                continue;
            }
            YamlNode value;
            if (rest.empty()) {
                if (i < ls.size() && ls[i].indent > indent) {
                    value = parse_block(ls, i, ls[i].indent);
                }
            } else {
                if (rest.front() == '[' && rest.back() == ']') {
                    value.kind = YamlNode::Kind::List;
                    for (const std::string& item :
                         split_commas(rest.substr(1, rest.size() - 2))) {
                        value.list.push_back(unquote(item));
                    }
                } else {
                    value.kind = YamlNode::Kind::Scalar;
                    value.scalar = unquote(rest);
                }
                /* Отступ после значения — это уже не наш формат. Молча
                 * выбросить строки значило бы потерять данные; принять их
                 * как продолжение — значит выдумать для них значение. */
                if (i < ls.size() && ls[i].indent > indent) {
                    diag(no, "строка «" + key + "» уже имеет значение, "
                             "следующие строки с отступом не поддерживаются");
                    skip_deeper(ls, i, indent);
                }
            }
            set_key(&node, key, std::move(value), no);
        }
        return node;
    }

private:
    void skip_deeper(const std::vector<SrcLine>& ls, size_t& i, size_t indent) {
        while (i < ls.size() && ls[i].indent > indent) ++i;
    }

    /* Повтор ключа: последнее значение выигрывает (как в YAML), но о
     * повторе сообщаем — два разных промпта в одном файле это ошибка
     * человека, а не его намерение. */
    void set_key(YamlNode* node, const std::string& key, YamlNode value,
                 size_t line_no) {
        for (auto& kv : node->map) {
            if (kv.first != key) continue;
            diag(line_no, "ключ «" + key + "» повторяется, взято последнее значение");
            kv.second = std::move(value);
            return;
        }
        node->map.emplace_back(key, std::move(value));
    }

    std::vector<AgentLoadDiag>* diags_;
    std::string path_;
};

/* Строки файла без завершающих \r. \r снимается ДО разбора, а не после:
 * конфиг мог быть написан на другой системе, и `mode: subagent\r` не
 * совпало бы ни с одним режимом — агент молча исчез бы из списка. */
std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') { out.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    /* \r снимается у КАЖДОЙ строки, а не только у строк frontmatter:
     * иначе `mode: subagent\r` не совпал бы ни с одним режимом (агент
     * молча исчез бы из списка), а в теле остался бы невидимый символ,
     * который ушёл бы в промпт. */
    for (std::string& line : out) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
    }
    return out;
}

std::string join_lines(const std::vector<std::string>& ls, size_t from) {
    std::string out;
    for (size_t i = from; i < ls.size(); ++i) {
        if (i > from) out += "\n";
        out += ls[i];
    }
    return out;
}

/* Ключи, которые И8.2 понимает. Всё остальное — в options (И8.3). */
bool is_known_key(const std::string& key) {
    static const char* kKnown[] = {
        "description", "mode", "model", "temperature", "top_p", "prompt",
        "tools", "permission", "steps", "color", "hidden",
    };
    for (const char* k : kKnown) {
        if (key == k) return true;
    }
    return false;
}

/* Один неизвестный ключ → options.
 *
 * Карта разворачивается в имя через точку: `options: {retries: "2"}`
 * читается как `retries`, `a: {b: 1}` — как `a.b`. Так ключи остаются
 * строками (ими их и читают: человек, параметр запроса, И8.3), а
 * вложенность не теряется и не требует второго типа в options. Глубже
 * одного уровня разворачивать незачем: у описания агента таких данных
 * нет, и молча принять третью вложенность значило бы изобрести формат,
 * который потом не прочитает никто. */
void collect_option(const std::string& key, const YamlNode& node,
                    std::map<std::string, std::string>* options,
                    const std::function<void(const std::string&)>& note) {
    if (node.kind == YamlNode::Kind::Scalar) {
        (*options)[key] = node.scalar;
        return;
    }
    if (node.kind == YamlNode::Kind::List) {
        /* Список склеивается запятой: options читаются как строки, и
         * отдельный тип «строка или список» в них не окупился бы. */
        (*options)[key] = join_comma(node.list);
        return;
    }
    for (const auto& kv : node.map) {
        if (kv.second.kind == YamlNode::Kind::Map) {
            note(key + "." + kv.first +
                 ": вложенность глубже одного уровня не поддерживается, "
                 "поддерево пропущено");
            continue;
        }
        collect_option(key + "." + kv.first, kv.second, options, note);
    }
}

} // namespace

bool parse_agent_markdown(const std::string& text, const std::string& name,
                          AgentDef* out, std::vector<AgentLoadDiag>* diags,
                          const std::string& source_path) {
    /* Замечание обязано называть ФАЙЛ, а не агента: «что-то не так с
     * README» и «что-то не так с агентом explore» — разные вещи, и по
     * первому чинит папку, по второму — агента. */
    const std::string& where = source_path.empty() ? name : source_path;
    auto note = [&](const std::string& message) {
        if (!diags) return;
        AgentLoadDiag d;
        d.path = where;
        d.message = message;
        diags->push_back(d);
    };

    const std::vector<std::string> raw = split_lines(text);
    if (raw.empty() || trim_ws(raw[0]) != "---") {
        note("нет frontmatter: файл должен начинаться со строки «---»");
        return false;
    }
    size_t end = 0;
    for (end = 1; end < raw.size(); ++end) {
        const std::string t = trim_ws(raw[end]);
        if (t == "---" || t == "...") break;
    }
    if (end >= raw.size()) {
        note("frontmatter не закрыт: нет строки «---»");
        return false;
    }

    /* Строки frontmatter: отступы считаются по пробелам, табы в отступе
     * недопустимы (YAML их запрещает, а наш подсчёт отступа обязан быть
     * однозначным). */
    std::vector<SrcLine> lines;
    for (size_t i = 1; i < end; ++i) {
        const std::string& r = raw[i];
        size_t indent = 0;
        while (indent < r.size() && (r[indent] == ' ' || r[indent] == '\t')) {
            if (r[indent] == '\t') {
                note("строка " + std::to_string(i + 1) +
                     ": табуляция в отступе не поддерживается");
            }
            ++indent;
        }
        const std::string body = trim_ws(r);
        if (body.empty() || body[0] == '#') continue;   /* пустая и комментарий */
        SrcLine sl;
        sl.indent = indent;
        sl.text = body;
        sl.number = i + 1;
        lines.push_back(sl);
    }

    Parser parser(diags, where);
    size_t pos = 0;
    const YamlNode root = parser.parse_block(lines, pos, 0);
    if (pos < lines.size()) {
        note("строка " + std::to_string(lines[pos].number) +
             ": неожиданный отступ, содержимое не разобрано");
    }

    out->name = name;
    out->source_path = where;
    bool fatal = false;

    /* mode разбирается первым и единственным отказом: он решает, может
     * ли агент править файлы, и «неизвестное значение → all» превратило
     * бы опечатку в разрешение. */
    const YamlNode* mode = root.find("mode");
    if (mode && !mode->scalar.empty()) {
        if (mode->scalar == "primary") out->mode = AgentMode::Primary;
        else if (mode->scalar == "subagent") out->mode = AgentMode::Subagent;
        else if (mode->scalar == "all") out->mode = AgentMode::All;
        else {
            note("mode: неизвестное значение «" + mode->scalar +
                 "» (допустимы primary, subagent, all) — агент не зарегистрирован");
            fatal = true;
        }
    }

    if (const YamlNode* d = root.find("description")) {
        if (d->kind == YamlNode::Kind::Scalar) out->description = d->scalar;
        else note("description: ожидалась строка");
    }
    if (out->description.empty()) {
        note("не задано description — агент попадёт в список без пояснения");
    }
    if (const YamlNode* m = root.find("model")) {
        if (m->kind == YamlNode::Kind::Scalar) out->model = m->scalar;
    }
    if (const YamlNode* c = root.find("color")) {
        if (c->kind == YamlNode::Kind::Scalar) out->color = c->scalar;
    }
    if (const YamlNode* h = root.find("hidden")) {
        bool v = false;
        if (h->kind == YamlNode::Kind::Scalar && parse_bool(h->scalar, &v)) {
            out->hidden = v;
        } else {
            note("hidden: ожидалось true/false, а не «" + h->scalar + "»");
        }
    }
    if (const YamlNode* t = root.find("temperature")) {
        double v = 0.0;
        if (t->kind == YamlNode::Kind::Scalar && parse_double(t->scalar, &v)) {
            out->has_temperature = true;
            out->temperature = v;
        } else {
            note("temperature: ожидалось число, а не «" + t->scalar + "»");
        }
    }
    if (const YamlNode* t = root.find("top_p")) {
        double v = 0.0;
        if (t->kind == YamlNode::Kind::Scalar && parse_double(t->scalar, &v)) {
            out->has_top_p = true;
            out->top_p = v;
        } else {
            note("top_p: ожидалось число, а не «" + t->scalar + "»");
        }
    }
    if (const YamlNode* s = root.find("steps")) {
        int v = 0;
        if (s->kind == YamlNode::Kind::Scalar && parse_int(s->scalar, &v)) {
            out->has_steps = true;
            out->steps = v;
        } else {
            note("steps: ожидалось целое, а не «" + s->scalar + "»");
        }
    }

    /* tools: карта «инструмент → можно» либо список разрешённых. */
    if (const YamlNode* t = root.find("tools")) {
        if (t->kind == YamlNode::Kind::Map) {
            for (const auto& kv : t->map) {
                if (kv.second.kind != YamlNode::Kind::Scalar) {
                    note("tools." + kv.first + ": ожидалось true/false");
                    continue;
                }
                bool v = false;
                if (!parse_bool(kv.second.scalar, &v)) {
                    note("tools." + kv.first + ": «" + kv.second.scalar +
                         "» — не true и не false");
                    continue;
                }
                out->tools[kv.first] = v;
            }
        } else if (t->kind == YamlNode::Kind::List) {
            for (const std::string& tool : t->list) out->tools[tool] = true;
        } else {
            note("tools: ожидалась карта «инструмент: true/false» или список");
        }
    }

    /* permission: ключ → действие или ключ → паттерн → действие.
     * Порядок объявления сохраняется: last match wins (core/permission.h),
     * и перестановка правил здесь означала бы другую конфигурацию. */
    if (const YamlNode* p = root.find("permission")) {
        if (p->kind != YamlNode::Kind::Map) {
            note("permission: ожидалась карта правил");
        } else {
            for (const auto& kv : p->map) {
                if (kv.second.kind == YamlNode::Kind::Scalar) {
                    PermissionEntry e;
                    e.key = kv.first;
                    e.action = PermissionAction::Ask;
                    if (!parse_action(kv.second.scalar, &e.action)) {
                        note("permission." + kv.first + ": «" + kv.second.scalar +
                             "» — не allow, не ask и не deny");
                        continue;
                    }
                    out->permission.push_back(e);
                    continue;
                }
                if (kv.second.kind != YamlNode::Kind::Map) {
                    note("permission." + kv.first + ": ожидалось действие или карта «паттерн: действие»");
                    continue;
                }
                for (const auto& pat : kv.second.map) {
                    PermissionEntry e;
                    e.key = kv.first;
                    e.pattern = pat.first.empty() ? "*" : pat.first;
                    e.action = PermissionAction::Ask;
                    if (pat.second.kind != YamlNode::Kind::Scalar ||
                        !parse_action(pat.second.scalar, &e.action)) {
                        note("permission." + kv.first + "." + pat.first +
                             ": действие должно быть allow, ask или deny");
                        continue;
                    }
                    out->permission.push_back(e);
                }
            }
        }
    }

    /* `options:` — известная группа, а не неизвестный ключ: её карта
     * разворачивается в имена БЕЗ префикса, иначе опция называлась бы
     * «options.retries» — то есть так, как её не назовёт ни человек,
     * ни И8.3. */
    if (const YamlNode* o = root.find("options")) {
        if (o->kind != YamlNode::Kind::Map) {
            note("options: ожидалась карта «имя: значение»");
        } else {
            for (const auto& kv : o->map) {
                collect_option(kv.first, kv.second, &out->options, note);
            }
        }
    }
    /* Неизвестные ключи — данные, а не мусор: И8.3 решит, что с ними. */
    for (const auto& kv : root.map) {
        if (is_known_key(kv.first)) continue;
        collect_option(kv.first, kv.second, &out->options, note);
    }

    /* Промпт: тело файла, а ключ `prompt` перекрывает его. */
    const std::string body = join_lines(raw, end + 1);
    const YamlNode* prompt = root.find("prompt");
    const std::string inline_prompt =
        (prompt && prompt->kind == YamlNode::Kind::Scalar) ? prompt->scalar : "";
    if (!inline_prompt.empty()) {
        if (!trim_ws(body).empty()) {
            note("заданы и ключ prompt, и тело файла — взято значение prompt");
        }
        out->prompt = inline_prompt;
    } else {
        out->prompt = body;
        if (trim_ws(body).empty()) {
            note("пустой промпт: тело файла и ключ prompt пусты");
        }
    }

    return !fatal;
}

std::vector<AgentLoadDiag> load_agents_from_directory(
        const std::string& dir, std::vector<AgentDef>* out) {
    std::vector<AgentLoadDiag> diags;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return diags;   /* каталога нет — не ошибка */

    std::vector<fs::path> files;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec) continue;
        std::string fname = it->path().filename().string();
        std::string lower = fname;
        for (char& c : lower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (lower.size() < 4 || lower.compare(lower.size() - 3, 3, ".md") != 0) continue;
        files.push_back(it->path());
    }
    /* Порядок чтения каталога не задан, а результат подмены по имени
     * («Foo.md» против «foo.md») от него зависит. Сортировка делает
     * «побеждает более поздний» воспроизводимым между прогонами. */
    std::sort(files.begin(), files.end());

    for (const fs::path& path : files) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            diags.push_back({path.string(), "файл не читается"});
            continue;
        }
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        std::string stem = path.filename().string();
        stem = stem.substr(0, stem.size() - 3);
        AgentDef def;
        /* Разбор смотрит только на содержимое; имя проверяет реестр —
         * правило имени должно быть одно (см. объявление). */
        if (parse_agent_markdown(text, stem, &def, &diags, path.string())) {
            out->push_back(std::move(def));
        }
    }
    return diags;
}

Ruleset normalized_agent_rules(const AgentDef& def,
                               std::vector<AgentLoadDiag>* diags) {
    Ruleset rules;
    auto note = [&](const std::string& message) {
        if (!diags) return;
        AgentLoadDiag d;
        d.path = def.source_path.empty() ? def.name : def.source_path;
        d.message = message;
        diags->push_back(d);
    };
    auto add_rule = [&](const std::string& raw_key, const std::string& pattern,
                        PermissionAction action, const std::string& origin) {
        bool known = false;
        const std::string key = canonical_permission_key(raw_key, &known);
        if (!known) {
            note(origin + ": «" + raw_key + "» — неизвестный инструмент или ключ "
                           "разрешения; правило сохранено и сработает, если "
                           "инструмент появится");
        } else if (key != raw_key) {
            note(origin + ": «" + raw_key + "» приведён к ключу «" + key + "»");
        }
        if (pattern.empty()) return;
        rules.add(key, pattern, action);
    };

    /* tools: карта «инструмент → можно». */
    for (const auto& kv : def.tools) {
        add_rule(kv.first, "*",
                 kv.second ? PermissionAction::Allow : PermissionAction::Deny,
                 "tools." + kv.first);
    }
    /* permission: после tools — перекрывает (см. шапку, п. 2). */
    for (const PermissionEntry& e : def.permission) {
        add_rule(e.key, e.pattern, e.action, "permission." + e.key);
    }
    return rules;
}

/* ======================================================================
 * И8.4: рантайм-структура агента
 * ====================================================================== */

namespace agent {

std::shared_ptr<Info> Info::from_def(const AgentDef& def, const Ruleset& base,
                                     std::vector<AgentLoadDiag>* diags) {
    return std::shared_ptr<Info>(new Info(def, base, diags));
}

Info::Info(const AgentDef& def, const Ruleset& base,
           std::vector<AgentLoadDiag>* diags) {
    name_ = def.name;
    description_ = def.description;
    prompt_ = def.prompt;
    model_ = def.model;
    color_ = def.color;
    source_path_ = def.source_path;
    options_ = def.options;
    mode_ = def.mode;
    hidden_ = def.hidden;
    has_temperature_ = def.has_temperature;
    temperature_ = def.temperature;
    has_top_p_ = def.has_top_p;
    top_p_ = def.top_p;
    /* Шаги: 0 — не заданы, и это НЕ «ноль шагов». Отрицательное и
     * нулевое значение в конфиге — опечатка, а не запрет работать: без
     * различения агент с `steps: 0` не сделал бы ничего, и это
     * выглядело бы как поломка, а не как «не задано». */
    if (def.has_steps) {
        steps_ = def.steps;
        if (def.steps < 0) {
            steps_ = 0;
            if (diags) {
                diags->push_back({def.source_path.empty() ? def.name : def.source_path,
                                  "steps: отрицательное значение («" +
                                      std::to_string(def.steps) +
                                      "») проигнорировано, взяты шаги сессии"});
            }
        }
    }

    /* Порядок правил решает всё: база первой, правила агента вторыми. */
    rules_ = base;
    const Ruleset own = normalized_agent_rules(def, diags);
    for (const Rule& r : own.rules()) rules_.add(r);

}

PermissionAction Info::evaluate(const std::string& key,
                                const std::string& pattern) const {
    /* За lock берётся только `approved`, а чтение `rules_` блокировки не
     * требует — она не меняется. */
    std::lock_guard<std::mutex> lk(approved_mtx_);
    bool matched = false;
    const PermissionAction a = approved_.evaluate_matched(key, pattern, &matched);
    /* Наложение, а не замена: «всегда на один путь» не имеет права
     * превращать «запретить» в «спросить» для всех остальных путей. */
    if (matched) return a;
    return rules_.evaluate(key, pattern);
}

bool Info::denies_whole_key(const std::string& key) const {
    return evaluate(key, "*") == PermissionAction::Deny;
}

void Info::approve(const std::string& key, const std::string& pattern) {
    std::lock_guard<std::mutex> lk(approved_mtx_);
    approved_.add(key, pattern.empty() ? "*" : pattern, PermissionAction::Allow);
}

} // namespace agent

std::vector<AgentLoadDiag> AgentRegistry::load_directory(const std::string& dir) {
    std::vector<AgentDef> defs;
    std::vector<AgentLoadDiag> diags = load_agents_from_directory(dir, &defs);
    for (const AgentDef& d : defs) {
        std::string why;
        if (add(d, &why)) continue;
        diags.push_back({d.source_path.empty() ? dir : d.source_path, why});
    }
    return diags;
}

} // namespace coder
