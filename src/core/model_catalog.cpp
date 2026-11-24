#include "core/model_catalog.h"

#include <nlohmann/json.hpp>

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace llama_gui {
namespace core {

namespace {

using nlohmann::json;

constexpr const char* kCatalogUrl =
    "https://models.opencode.ai/experimental/v2.0/api.json"
    "?type=chat,decision,image";

constexpr const char* kUserAgent = "llama-gui/model-catalog";

double json_number(const json& j) {
    if (j.is_number()) return j.get<double>();
    if (j.is_string()) {
        try {
            return std::stod(j.get<std::string>());
        } catch (...) {
            return 0.0;
        }
    }
    return 0.0;
}

double number_or(const json& obj, const char* key, double fallback = 0.0) {
    if (!obj.is_object() || !obj.contains(key)) return fallback;
    return json_number(obj[key]);
}

int int_or(const json& obj, const char* key, int fallback = 0) {
    return static_cast<int>(number_or(obj, key, static_cast<double>(fallback)));
}

bool bool_or(const json& obj, const char* key, bool fallback = false) {
    if (!obj.is_object() || !obj.contains(key)) return fallback;
    const auto& v = obj[key];
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0.0;
    return fallback;
}

/* base_url чата провайдера берётся из model.api["chat-completions"].base_url;
 * если такой секции нет — первый найденный base_url. */
std::string extract_base_url(const json& model) {
    if (!model.contains("api")) return {};
    const json& api = model["api"];
    if (api.is_string()) return api.get<std::string>();
    if (!api.is_object()) return {};
    auto it = api.find("chat-completions");
    if (it != api.end() && it->is_object() && it->contains("base_url") &&
        (*it)["base_url"].is_string()) {
        return (*it)["base_url"].get<std::string>();
    }
    for (auto cit = api.begin(); cit != api.end(); ++cit) {
        if (cit->is_object() && cit->contains("base_url") &&
            (*cit)["base_url"].is_string()) {
            return (*cit)["base_url"].get<std::string>();
        }
    }
    return {};
}

bool extract_tools(const json& model) {
    if (!model.contains("capabilities")) return false;
    const json& caps = model["capabilities"];
    if (!caps.is_object() || !caps.contains("tools")) return false;
    const json& tools = caps["tools"];
    if (tools.is_boolean()) return tools.get<bool>();
    if (tools.is_object() && tools.contains("supported")) {
        return tools["supported"].is_boolean() && tools["supported"].get<bool>();
    }
    return false;
}

/* Отличает «инструменты не поддерживаются» от «в каталоге о них ничего
 * нет»: в первом случае нативный вызов можно не слать вовсе, во втором —
 * обязана решать проба (NativeToolSupport::Unknown). */
bool extract_tools_known(const json& model) {
    if (!model.contains("capabilities")) return false;
    const json& caps = model["capabilities"];
    if (!caps.is_object() || !caps.contains("tools")) return false;
    const json& tools = caps["tools"];
    if (tools.is_boolean()) return true;
    return tools.is_object() && tools.contains("supported") &&
           tools["supported"].is_boolean();
}

bool extract_reasoning(const json& model) {
    if (!model.contains("capabilities")) return false;
    const json& caps = model["capabilities"];
    if (!caps.is_object() || !caps.contains("reasoning")) return false;
    const json& r = caps["reasoning"];
    if (r.is_boolean()) return r.get<bool>();
    if (r.is_object() && r.contains("supported")) {
        return r["supported"].is_boolean() && r["supported"].get<bool>();
    }
    /* Наличие секции reasoning без явного supported трактуем как поддержку:
     * провайдер описывает параметры рассуждений, только если они есть. */
    return r.is_object();
}

size_t write_to_string(void* contents, size_t size, size_t nmemb, void* userp) {
    const size_t total = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), total);
    return total;
}

size_t capture_header(void* contents, size_t size, size_t nmemb, void* userp) {
    const size_t total = size * nmemb;
    std::string line(static_cast<char*>(contents), total);
    std::string lower = line;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (lower.rfind("etag:", 0) == 0) {
        std::string value = line.substr(5);
        value.erase(0, value.find_first_not_of(" \t"));
        while (!value.empty() &&
               (value.back() == '\r' || value.back() == '\n' || value.back() == ' '))
            value.pop_back();
        *static_cast<std::string*>(userp) = value;
    }
    return total;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool canonical_matches(const std::string& canonical, const std::string& id) {
    if (canonical.empty() || id.empty()) return false;
    return canonical == id || ends_with(canonical, "/" + id);
}

bool is_alnum(unsigned char c) { return std::isalnum(c) != 0; }

} // namespace

// ============================================================================
// Чистые помощники
// ============================================================================

std::string normalize_endpoint(const std::string& url) {
    std::string s = url;
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    s = s.substr(b, e - b + 1);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    while (!s.empty() && s.back() == '/') s.pop_back();
    const std::string tail = "/chat/completions";
    if (ends_with(s, tail)) {
        s.erase(s.size() - tail.size());
        while (!s.empty() && s.back() == '/') s.pop_back();
    }
    return s;
}

std::string normalize_model_name(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    bool last_dash = false;
    for (char ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (is_alnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
            last_dash = false;
        } else if (!out.empty() && !last_dash) {
            out.push_back('-');
            last_dash = true;
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

std::vector<CatalogModel> parse_catalog(const std::string& raw_json) {
    std::vector<CatalogModel> out;
    if (raw_json.empty()) return out;
    json root;
    try {
        root = json::parse(raw_json);
    } catch (const std::exception& e) {
        std::cerr << "[ModelCatalog] parse failed: " << e.what() << std::endl;
        return out;
    }
    if (!root.is_object()) return out;
    for (auto pit = root.begin(); pit != root.end(); ++pit) {
        if (!pit->is_object()) continue;
        const json& provider = *pit;
        const std::string provider_id = pit.key();
        const std::string provider_name =
            provider.value("name", provider_id);
        if (!provider.contains("models") || !provider["models"].is_object())
            continue;
        for (auto mit = provider["models"].begin();
             mit != provider["models"].end(); ++mit) {
            if (!mit->is_object()) continue;
            const json& m = *mit;
            CatalogModel cm;
            cm.provider = provider_id;
            cm.provider_name = provider_name;
            cm.model_id = m.value("id", mit.key());
            cm.canonical_id = m.value("canonical_id", m.value("canonical_model_id", ""));
            cm.name = m.value("name", cm.model_id);
            cm.family = m.value("family", "");
            cm.base_url = extract_base_url(m);
            cm.tools_supported = extract_tools(m);
            cm.tools_known = extract_tools_known(m);
            cm.reasoning = extract_reasoning(m);
            if (m.contains("limit") && m["limit"].is_object()) {
                cm.context = int_or(m["limit"], "context");
                cm.max_output = int_or(m["limit"], "output");
            }
            if (m.contains("cost") && m["cost"].is_object()) {
                const json& cost = m["cost"];
                if (cost.contains("text") && cost["text"].is_object()) {
                    const json& text = cost["text"];
                    if (text.contains("input") && text["input"].is_object()) {
                        cm.price_in = number_or(text["input"], "price");
                        cm.has_cost = true;
                        if (text["input"].contains("cache") &&
                            text["input"]["cache"].is_object()) {
                            cm.cache_read = number_or(text["input"]["cache"], "read");
                            cm.cache_write = number_or(text["input"]["cache"], "write");
                        }
                    }
                    if (text.contains("output") && text["output"].is_object()) {
                        cm.price_out = number_or(text["output"], "price");
                        cm.has_cost = true;
                    }
                }
            }
            out.push_back(std::move(cm));
        }
    }
    return out;
}

namespace {

void fill_info(CatalogModelInfo& info, const CatalogModel& m, const std::string& source) {
    info.found = true;
    info.source = source;
    info.provider = m.provider;
    info.model_id = m.model_id;
    info.canonical_id = m.canonical_id;
    info.family = m.family;
    info.tools_supported = m.tools_supported;
    info.tools_known = m.tools_known;
    info.reasoning = m.reasoning;
    info.context = m.context;
    info.max_output = m.max_output;
    info.price_in = m.price_in;
    info.price_out = m.price_out;
    info.cache_read = m.cache_read;
    info.cache_write = m.cache_write;
    info.has_cost = m.has_cost;
}

} // namespace

CatalogModelInfo match_model(const std::vector<CatalogModel>& models,
                      const std::string& endpoint_url,
                      const std::string& model_id) {
    CatalogModelInfo info;
    info.model_id = model_id;
    if (model_id.empty()) return info;

    const std::string endpoint = normalize_endpoint(endpoint_url);

    auto endpoint_match = [&](const CatalogModel& m) {
        return !endpoint.empty() && normalize_endpoint(m.base_url) == endpoint;
    };

    /* (1) точное совпадение endpoint + model_id. */
    for (const auto& m : models) {
        if (endpoint_match(m) && m.model_id == model_id) {
            fill_info(info, m, "exact");
            return info;
        }
    }

    /* (2) canonical_model_id по суффиксу с приоритетом провайдера из (1). */
    for (const auto& m : models) {
        if (endpoint_match(m) && canonical_matches(m.canonical_id, model_id)) {
            fill_info(info, m, "exact");
            return info;
        }
    }

    /* (3) любой провайдер с тем же model_id / canonical_model_id. */
    for (const auto& m : models) {
        if (m.model_id == model_id) {
            fill_info(info, m, "reference");
            return info;
        }
    }
    for (const auto& m : models) {
        if (canonical_matches(m.canonical_id, model_id)) {
            fill_info(info, m, "reference");
            return info;
        }
    }

    /* (4) нормализованное совпадение имени. */
    const std::string wanted = normalize_model_name(model_id);
    if (!wanted.empty()) {
        for (const auto& m : models) {
            if (normalize_model_name(m.model_id) == wanted ||
                normalize_model_name(m.name) == wanted) {
                fill_info(info, m, "reference");
                return info;
            }
        }
    }
    return info;
}

CatalogModelInfo match_local_model(const std::vector<CatalogModel>& models,
                            const std::string& model_path) {
    std::string stem;
    if (!model_path.empty()) {
        stem = std::filesystem::path(model_path).stem().string();
        if (stem.empty()) stem = model_path;
    }
    CatalogModelInfo info = match_model(models, "", stem);
    /* Филенейм GGUF содержит квантование и вариант: `qwen3-32b.Q4_K_M`.
     * Точного совпадения имени не будет, поэтому если не нашли — ищем
     * модель, чьё нормализованное имя является префиксом имени файла. */
    if (!info.found && !stem.empty()) {
        const std::string normalized = normalize_model_name(stem);
        for (const auto& m : models) {
            const std::string candidate = normalize_model_name(m.model_id);
            if (candidate.size() >= 3 && normalized.rfind(candidate, 0) == 0) {
                fill_info(info, m, "local");
                break;
            }
        }
    }
    /* Локальная модель по определению не «exact» из каталога: её нет у
     * провайдера. source сообщает хосту, что это локальный путь, а found —
     * что нашлись справочные подсказки. */
    info.source = "local";
    return info;
}

int resolve_context_window(const CatalogModelInfo& info,
                           int profile_context,
                           int local_n_ctx) {
    /* Совпадение endpoint + model_id — запись именно того провайдера,
     * через которого идут запросы; ей верим больше профиля. */
    if (info.source == "exact" && info.context > 0) return info.context;
    /* Профиль провайдера: окно выбранной модели, присланное его /models.
     * Точнее справочного совпадения по имени, потому что провайдер у нас
     * один и тот же. */
    if (profile_context > 0) return profile_context;
    /* Справочное совпадение: лучше, чем «неизвестно», и это advertised
     * окно модели — то, что человек и ожидает видеть в метриках. */
    if (info.context > 0) return info.context;
    /* Локальный n_ctx — фактически загруженное окно llama-server.
     * Подставляется только когда каталог молчит: иначе у известной
     * модели с n_ctx=4096 панель писала бы 4096 вместо её 131072. */
    if (local_n_ctx > 0) return local_n_ctx;
    return 0;
}

// ============================================================================
// CatalogModelInfo::to_json
// ============================================================================

std::string CatalogModelInfo::to_json() const {
    json j;
    j["ok"] = 1;
    j["found"] = found;
    j["source"] = source;
    j["provider"] = provider;
    j["model_id"] = model_id;
    j["canonical_model_id"] = canonical_id;
    j["family"] = family;
    j["tools_supported"] = tools_supported;
    j["tools_known"] = tools_known;
    j["reasoning"] = reasoning;
    j["context"] = context;
    j["max_output"] = max_output;
    j["price_in"] = price_in;
    j["price_out"] = price_out;
    j["cache_read"] = cache_read;
    j["cache_write"] = cache_write;
    j["has_cost"] = has_cost;
    return j.dump();
}

// ============================================================================
// ModelCatalog
// ============================================================================

ModelCatalog& ModelCatalog::instance() {
    static ModelCatalog catalog;
    return catalog;
}

std::string ModelCatalog::cache_path() const {
    const char* home = std::getenv("HOME");
    std::string base = home ? std::string(home) : std::string(".");
    return base + "/.llama-gui/models_dev.json";
}

std::string ModelCatalog::etag_path() const {
    const char* home = std::getenv("HOME");
    std::string base = home ? std::string(home) : std::string(".");
    return base + "/.llama-gui/models_dev.etag";
}

void ModelCatalog::load_cache_locked() {
    std::ifstream in(cache_path());
    if (!in.is_open()) return;
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string raw = ss.str();
    if (raw.empty()) return;

    std::vector<CatalogModel> loaded;
    try {
        const json root = json::parse(raw);
        if (root.is_object() && root.contains("models") && root["models"].is_array()) {
            for (const auto& m : root["models"]) {
                if (!m.is_object()) continue;
                CatalogModel cm;
                cm.provider = m.value("provider", "");
                cm.provider_name = m.value("provider_name", "");
                cm.model_id = m.value("model_id", "");
                cm.canonical_id = m.value("canonical_id", "");
                cm.name = m.value("name", "");
                cm.family = m.value("family", "");
                cm.base_url = m.value("base_url", "");
                cm.tools_supported = m.value("tools_supported", false);
                cm.tools_known = m.value("tools_known", false);
                cm.reasoning = m.value("reasoning", false);
                cm.context = m.value("context", 0);
                cm.max_output = m.value("max_output", 0);
                cm.price_in = m.value("price_in", 0.0);
                cm.price_out = m.value("price_out", 0.0);
                cm.cache_read = m.value("cache_read", 0.0);
                cm.cache_write = m.value("cache_write", 0.0);
                cm.has_cost = m.value("has_cost", false);
                loaded.push_back(std::move(cm));
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[ModelCatalog] cache parse failed: " << e.what() << std::endl;
        return;
    }
    if (!loaded.empty()) {
        models_ = std::move(loaded);
    }

    std::ifstream ef(etag_path());
    if (ef.is_open()) {
        std::getline(ef, etag_);
    }
}

void ModelCatalog::save_cache(const std::vector<CatalogModel>& models,
                              const std::string& etag) {
    try {
        std::filesystem::create_directories(
            std::filesystem::path(cache_path()).parent_path());
    } catch (const std::exception&) {
    }

    json root;
    root["fetched_at"] = static_cast<long long>(std::time(nullptr));
    json arr = json::array();
    for (const auto& m : models) {
        json j;
        j["provider"] = m.provider;
        j["provider_name"] = m.provider_name;
        j["model_id"] = m.model_id;
        if (!m.canonical_id.empty()) j["canonical_id"] = m.canonical_id;
        if (!m.name.empty()) j["name"] = m.name;
        if (!m.family.empty()) j["family"] = m.family;
        if (!m.base_url.empty()) j["base_url"] = m.base_url;
        j["tools_supported"] = m.tools_supported;
        j["tools_known"] = m.tools_known;
        j["reasoning"] = m.reasoning;
        j["context"] = m.context;
        j["max_output"] = m.max_output;
        j["price_in"] = m.price_in;
        j["price_out"] = m.price_out;
        j["cache_read"] = m.cache_read;
        j["cache_write"] = m.cache_write;
        j["has_cost"] = m.has_cost;
        arr.push_back(std::move(j));
    }
    root["models"] = std::move(arr);

    std::ofstream out(cache_path(), std::ios::trunc);
    if (out.is_open()) out << root.dump();

    if (!etag.empty()) {
        std::ofstream ef(etag_path(), std::ios::trunc);
        if (ef.is_open()) ef << etag;
    }
}

void ModelCatalog::ensure_loaded() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!loaded_.exchange(true)) {
            load_cache_locked();
        }
    }
    refresh_async();
}

void ModelCatalog::refresh_now() {
    std::string etag;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        etag = etag_;
    }

    CURL* curl = curl_easy_init();
    if (!curl) return;

    std::string body;
    std::string response_etag;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Accept-Encoding: gzip");
    if (!etag.empty()) {
        headers = curl_slist_append(headers, ("If-None-Match: " + etag).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, kCatalogUrl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, capture_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_etag);

    const CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::cerr << "[ModelCatalog] fetch failed: " << curl_easy_strerror(res)
                  << " (using cache)" << std::endl;
        return;
    }
    if (http_code == 304) {
        return; // кэш актуален
    }
    if (http_code != 200 || body.empty()) {
        std::cerr << "[ModelCatalog] unexpected HTTP " << http_code << std::endl;
        return;
    }

    std::vector<CatalogModel> parsed = parse_catalog(body);
    if (parsed.empty()) return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        models_ = parsed;
        if (!response_etag.empty()) etag_ = response_etag;
    }
    save_cache(parsed, response_etag);
}

void ModelCatalog::refresh_async(std::function<void()> done) {
    if (refresh_started_.exchange(true)) return;
    std::thread([this, done]() {
        refresh_now();
        if (done) done();
    }).detach();
}

CatalogModelInfo ModelCatalog::lookup(const std::string& endpoint_url,
                               const std::string& model_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return match_model(models_, endpoint_url, model_id);
}

CatalogModelInfo ModelCatalog::lookup_local(const std::string& model_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return match_local_model(models_, model_path);
}

bool ModelCatalog::lookup_price(const std::string& endpoint_url,
                                const std::string& model_id,
                                double& price_in, double& price_out) const {
    const CatalogModelInfo info = lookup(endpoint_url, model_id);
    if (!info.found || !info.has_cost) return false;
    price_in = info.price_in;
    price_out = info.price_out;
    return true;
}

size_t ModelCatalog::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return models_.size();
}

CatalogModelInfo ModelCatalog::current_info(bool cloud,
                                            const std::string& endpoint_url,
                                            const std::string& model_id,
                                            const std::string& local_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cloud ? match_model(models_, endpoint_url, model_id)
                 : match_local_model(models_, local_path);
}

std::string ModelCatalog::current_json(bool cloud, const std::string& endpoint_url,
                                       const std::string& model_id,
                                       const std::string& local_path) const {
    CatalogModelInfo info = current_info(cloud, endpoint_url, model_id, local_path);
    json j = json::parse(info.to_json());
    j["kind"] = cloud ? "cloud" : "local";
    return j.dump();
}

} // namespace core
} // namespace llama_gui
