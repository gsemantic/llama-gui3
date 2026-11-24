/*
 * rag_registry.cpp — реестр скоупов RAG (R1.2).
 *
 * Обоснование и границы — в rag_registry.h. Здесь только код.
 */

#include "core/rag_registry.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace llama_gui {
namespace core {

namespace {

inline bool is_lower_alpha(char c) { return c >= 'a' && c <= 'z'; }
inline bool is_digit(char c) { return c >= '0' && c <= '9'; }
inline bool is_name_char(char c) {
    return is_lower_alpha(c) || is_digit(c) || c == '_' || c == '-';
}
inline bool is_suffix_char(char c) {
    return is_lower_alpha(c) || is_digit(c) || c == '_' || c == '-' || c == '.';
}

/* Имя плагина как идентификатор: `^[a-z][a-z0-9_-]{0,63}$`.
 *
 * Это те же правила, что у plugin_identity.h на стороне плагинов, но
 * здесь они нужны ВНУТРИ хоста: реестр не должен зависеть от заголовков
 * плагина (core не знает про plugins/). Дублирование намеренное и
 * проверяется тестом на согласованность. */
bool valid_plugin_name(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    if (!is_lower_alpha(s[0])) return false;
    for (size_t i = 1; i < s.size(); ++i) {
        if (!is_name_char(s[i])) return false;
    }
    return true;
}

bool valid_suffix(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    if (!is_lower_alpha(s[0]) && !is_digit(s[0])) return false;
    for (char c : s) {
        if (!is_suffix_char(c)) return false;
    }
    return true;
}

} // namespace

bool normalize_scope(const std::string& plugin_name,
                     const std::string& suffix,
                     std::string& scope_out,
                     std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };

    if (!valid_plugin_name(plugin_name)) {
        return fail("invalid plugin name: '" + plugin_name + "'");
    }
    if (!suffix.empty() && !valid_suffix(suffix)) {
        return fail("invalid suffix: '" + suffix + "'");
    }

    scope_out = "plugin:" + plugin_name;
    if (!suffix.empty()) scope_out += ":" + suffix;
    return true;
}

std::string scope_dir(const std::string& root, const std::string& scope) {
    if (root.empty() || scope.empty()) return "";

    /* Имя каталога: двоеточия и точки заменяются на подчёркивание —
     * `plugin:wp_coder:archive` → `plugin_wp_coder_archive`. Это не
     * косметика: двоеточие недопустимо в именах файлов Windows, а точка
     * создаёт ложное «расширение». */
    std::string dir_name;
    dir_name.reserve(scope.size());
    for (char c : scope) {
        if (c == ':' || c == '.') dir_name += '_';
        else if (is_name_char(c)) dir_name += c;
        else return "";   /* посторонний символ: лучше отказ, чем путь */
    }
    if (dir_name.empty()) return "";

    std::error_code ec;
    fs::path root_path = fs::weakly_canonical(fs::path(root), ec);
    if (ec) root_path = fs::path(root);
    fs::path full = root_path / dir_name;

    /* Проверка «внутри root» — последняя линия. weakly_canonical
     * убирает `..`, поэтому сравнение канонических путей надёжно: если
     * scope каким-то образом вышел наружу, full не начнётся с root. */
    std::string full_str = full.string();
    const std::string root_str = root_path.string();
    if (full_str.compare(0, root_str.size(), root_str) != 0) {
        return "";
    }
    return full_str;
}

bool parse_scope_options(const std::string& json,
                         RagScopeOptions& out,
                         std::string* error) {
    out = RagScopeOptions{};
    if (json.empty()) return true;   /* нет настроек = значения по умолчанию */

    nlohmann::json j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        if (error) *error = "scope options: not a JSON object";
        return false;
    }

    auto get_int = [&](const char* key, int& dst) {
        auto it = j.find(key);
        if (it == j.end()) return;
        if (!it->is_number_integer()) {
            /* Опечатка в профиле не должна ломать доступ: поле
             * игнорируется, значение остаётся «не задано». */
            std::cerr << "[RagRegistry] scope option '" << key
                      << "' is not an integer — ignored" << std::endl;
            return;
        }
        dst = it->get<int>();
    };

    get_int("max_documents", out.max_documents);
    if (out.max_documents < 0) out.max_documents = 0;

    auto it = j.find("max_index_bytes");
    if (it != j.end()) {
        if (it->is_number_integer()) {
            out.max_index_bytes = it->get<long long>();
            if (out.max_index_bytes < 0) out.max_index_bytes = 0;
        } else {
            std::cerr << "[RagRegistry] scope option 'max_index_bytes'"
                      << " is not an integer — ignored" << std::endl;
        }
    }

    auto get_bool = [&](const char* key, bool& dst) {
        auto i = j.find(key);
        if (i == j.end()) return;
        if (i->is_boolean()) dst = i->get<bool>();
        else std::cerr << "[RagRegistry] scope option '" << key
                       << "' is not a boolean — ignored" << std::endl;
    };

    get_bool("allow_index", out.allow_index);
    get_bool("read_only", out.read_only);

    /* read_only — это «не индексировать». Держать два независимых флага,
     * которые могут противоречить, нельзя: «читать можно, писать нельзя»
     * — единственное согласованное прочтение, и оно выводится ОТСЮДА. */
    if (out.read_only) out.allow_index = false;

    return true;
}

// ============================================================================
// RagRegistry
// ============================================================================

RagRegistry::RagRegistry(std::string root,
                         std::shared_ptr<EmbeddingGenerator> embedding,
                         size_t max_loaded)
    : root_(std::move(root)), embedding_(std::move(embedding)),
      max_loaded_(max_loaded > 0 ? max_loaded : 1) {
    std::error_code ec;
    if (!root_.empty() && !fs::exists(root_, ec)) {
        fs::create_directories(root_, ec);
        if (ec) {
            std::cerr << "[RagRegistry] cannot create " << root_
                      << ": " << ec.message() << std::endl;
        }
    }
}

RagRegistry::~RagRegistry() {
    std::lock_guard<std::mutex> lk(mtx_);
    /* Флаш всех загруженных сторов: деструктор стора сам флашит, но
     * порядок разрушения контейнеров не гарантирует, что RagManager ещё
     * жив. Явный flush_all() до разрушения — надёжнее. */
    for (auto& s : lru_) {
        if (s) s->flush();
    }
    lru_.clear();
}

std::shared_ptr<RagStore> RagRegistry::get(const std::string& plugin_name,
                                           const std::string& suffix,
                                           std::string* error) {
    std::string scope;
    std::string why;
    if (!normalize_scope(plugin_name, suffix, scope, &why)) {
        if (error) *error = why;
        return nullptr;
    }
    const std::string path = scope_dir(root_, scope);
    if (path.empty()) {
        if (error) *error = "scope dir is outside registry root";
        return nullptr;
    }

    std::lock_guard<std::mutex> lk(mtx_);
    return get_locked(scope, path, error);
}

std::shared_ptr<RagStore> RagRegistry::get_locked(const std::string& scope,
                                                  const std::string& path,
                                                  std::string* error) {
    (void)error;
    for (auto& s : lru_) {
        if (s && s->scope() == scope) {
            touch_locked(s.get());
            return s;
        }
    }

    auto store = std::make_shared<RagStore>(scope, path, embedding_);
    lru_.insert(lru_.begin(), store);
    evict_locked();
    return store;
}

void RagRegistry::touch_locked(RagStore* store) {
    /* Переместить в начало (самые свежие). */
    auto it = std::find_if(lru_.begin(), lru_.end(),
                           [store](const std::shared_ptr<RagStore>& s) {
                               return s.get() == store;
                           });
    if (it != lru_.end() && it != lru_.begin()) {
        std::shared_ptr<RagStore> held = *it;
        lru_.erase(it);
        lru_.insert(lru_.begin(), std::move(held));
    }
}

void RagRegistry::evict_locked() {
    /* Вытесняем самые старые, пока не влезем в max_loaded_. Вытесненный
     * стор флашится в деструкторе; если он занят другим потоком, shared_ptr
     * не освободится и стор просто переживёт вытеснение — это лучше, чем
     * упасть на живом индексе. */
    while (lru_.size() > max_loaded_) {
        lru_.pop_back();
    }
}

RagStoreInfo RagRegistry::info(const std::string& plugin_name,
                               const std::string& suffix) const {
    RagStoreInfo out;
    std::string scope;
    if (!normalize_scope(plugin_name, suffix, scope)) return out;
    out.scope = scope;
    out.path = scope_dir(root_, scope);

    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& s : lru_) {
        if (s && s->scope() == scope) return s->info();
    }
    /* Стор не загружен: заполняем то, что можно узнать с диска, не
     * создавая менеджер (иначе info() имел бы побочный эффект — загрузку
     * индекса). */
    out.reason = "индекс не загружен";
    out.bytes = 0;
    std::error_code ec;
    if (!out.path.empty() && fs::exists(out.path, ec)) {
        for (const auto& e : fs::directory_iterator(out.path, ec)) {
            if (e.is_regular_file()) out.bytes += static_cast<size_t>(fs::file_size(e.path(), ec));
        }
        out.reason = "индекс не загружен (файлы есть)";
    }
    return out;
}

std::vector<std::string> RagRegistry::list() const {
    std::vector<std::string> scopes;
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& s : lru_) {
        if (s) scopes.push_back(s->scope());
    }
    /* Плюс каталоги на диске, которых нет в памяти: после перезапуска
     * процесса это единственный способ узнать о прошлых индексах. */
    std::error_code ec;
    if (!root_.empty() && fs::exists(root_, ec)) {
        for (const auto& e : fs::directory_iterator(root_, ec)) {
            if (!e.is_directory()) continue;
            const std::string name = e.path().filename().string();
            /* Обратно в имя скоупа: подчёркивания могли быть и в имени
             * плагина, поэтому точное восстановление невозможно — и не
             * требуется: это список для UI, а не ключ. */
            const std::string scope = "plugin:" + name;
            if (std::find(scopes.begin(), scopes.end(), scope) == scopes.end()) {
                scopes.push_back(scope);
            }
        }
    }
    std::sort(scopes.begin(), scopes.end());
    return scopes;
}

bool RagRegistry::remove(const std::string& plugin_name, const std::string& suffix) {
    std::string scope;
    if (!normalize_scope(plugin_name, suffix, scope)) return false;

    std::lock_guard<std::mutex> lk(mtx_);
    /* Убираем из памяти ПЕРВЫМ: иначе деструктор стора после clear()
     * записал бы пустой индекс обратно. */
    for (auto it = lru_.begin(); it != lru_.end();) {
        if (*it && (*it)->scope() == scope) it = lru_.erase(it);
        else ++it;
    }
    const std::string path = scope_dir(root_, scope);
    if (path.empty()) return false;
    std::error_code ec;
    fs::remove_all(path, ec);
    return !ec;
}

void RagRegistry::flush_all() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& s : lru_) {
        if (s) s->flush();
    }
}

size_t RagRegistry::loaded_count() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return lru_.size();
}

} // namespace core
} // namespace llama_gui