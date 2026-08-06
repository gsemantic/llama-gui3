/**
 * @file web_search_agent.cpp
 * @brief Агент для HTTP запросов и поиска в интернете
 */

#include "web_search_agent.h"
#include <curl/curl.h>
#include <sstream>
#include <algorithm>

namespace agents {

// ============================================================================
// Вспомогательные функции для CURL
// ============================================================================

static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    std::string* buffer = static_cast<std::string*>(userp);
    
    buffer->append(static_cast<char*>(contents), realsize);
    return realsize;
}

// ============================================================================
// WebSearchAgent implementation
// ============================================================================

WebSearchAgent::WebSearchAgent() {
    curl_global_init(CURL_GLOBAL_ALL);
}

WebSearchAgent::~WebSearchAgent() {
    shutdown();
    curl_global_cleanup();
}

const char* WebSearchAgent::name() const {
    return "web_search_agent";
}

const char* WebSearchAgent::description() const {
    return "Web search and HTTP client agent. Supports GET, POST, PUT, DELETE "
           "requests with customizable headers and timeouts.";
}

const char* WebSearchAgent::version() const {
    return "1.0.0";
}

bool WebSearchAgent::initialize(AgentContext* context) {
    if (!context) {
        return false;
    }
    
    context_ = context;
    initialized_ = true;
    
    // Загрузка настроек
    auto config = context_->get_agent_config(name());
    if (config.contains("default_timeout_ms")) {
        timeout_ms_ = config["default_timeout_ms"].get<int>();
    }
    if (config.contains("user_agent")) {
        user_agent_ = config["user_agent"].get<std::string>();
    }
    if (config.contains("max_redirects")) {
        max_redirects_ = config["max_redirects"].get<int>();
    }
    
    context_->info(name(), "Initialized with timeout=" + 
                   std::to_string(timeout_ms_) + "ms");
    
    return true;
}

AgentResult WebSearchAgent::execute(const AgentRequest& request) {
    if (!initialized_) {
        return AgentResult::error("Agent not initialized");
    }
    
    std::string action = request.action();
    
    if (action == "get") {
        return handle_get(request);
    } else if (action == "post") {
        return handle_post(request);
    } else if (action == "put") {
        return handle_put(request);
    } else if (action == "delete") {
        return handle_delete(request);
    } else if (action == "search") {
        return handle_search(request);
    } else if (action == "head") {
        return handle_head(request);
    }
    
    return AgentResult::error("Unknown action: " + action);
}

void WebSearchAgent::shutdown() {
    if (context_) {
        context_->info(name(), "Shutting down");
    }
    initialized_ = false;
    context_ = nullptr;
}

AgentCapability WebSearchAgent::capabilities() const {
    return AgentCapability::HTTP_GET | 
           AgentCapability::HTTP_POST |
           AgentCapability::HTTP_PUT |
           AgentCapability::HTTP_DELETE |
           AgentCapability::WEB_SEARCH;
}

bool WebSearchAgent::is_ready() const {
    return initialized_;
}

// ============================================================================
// HTTP методы
// ============================================================================

AgentResult WebSearchAgent::handle_get(const AgentRequest& request) {
    std::string url = request.get_param<std::string>("url", "");
    
    if (url.empty()) {
        return AgentResult::error("URL is empty");
    }
    
    // Проверка URL через SecurityManager
    if (context_) {
        // auto result = context_->security()->check_url(name(), url);
        // if (!result.allowed) {
        //     return AgentResult::error(result.reason);
        // }
    }
    
    int timeout = request.get_param<int>("timeout_ms", timeout_ms_);
    std::string user_agent = request.get_param<std::string>("user_agent", user_agent_);
    
    // Заголовки
    std::vector<std::string> headers = request.get_param<std::vector<std::string>>("headers", {});
    
    return perform_request("GET", url, "", headers, timeout, user_agent);
}

AgentResult WebSearchAgent::handle_post(const AgentRequest& request) {
    std::string url = request.get_param<std::string>("url", "");
    std::string body = request.get_param<std::string>("body", "");
    
    if (url.empty()) {
        return AgentResult::error("URL is empty");
    }
    
    int timeout = request.get_param<int>("timeout_ms", timeout_ms_);
    std::string content_type = request.get_param<std::string>("content_type", "application/json");
    
    std::vector<std::string> headers;
    headers.push_back("Content-Type: " + content_type);
    
    auto custom_headers = request.get_param<std::vector<std::string>>("headers", {});
    headers.insert(headers.end(), custom_headers.begin(), custom_headers.end());
    
    return perform_request("POST", url, body, headers, timeout, user_agent_);
}

AgentResult WebSearchAgent::handle_put(const AgentRequest& request) {
    std::string url = request.get_param<std::string>("url", "");
    std::string body = request.get_param<std::string>("body", "");
    
    if (url.empty()) {
        return AgentResult::error("URL is empty");
    }
    
    int timeout = request.get_param<int>("timeout_ms", timeout_ms_);
    
    std::vector<std::string> headers;
    headers.push_back("Content-Type: application/json");
    
    return perform_request("PUT", url, body, headers, timeout, user_agent_);
}

AgentResult WebSearchAgent::handle_delete(const AgentRequest& request) {
    std::string url = request.get_param<std::string>("url", "");
    
    if (url.empty()) {
        return AgentResult::error("URL is empty");
    }
    
    int timeout = request.get_param<int>("timeout_ms", timeout_ms_);
    
    return perform_request("DELETE", url, "", {}, timeout, user_agent_);
}

AgentResult WebSearchAgent::handle_head(const AgentRequest& request) {
    std::string url = request.get_param<std::string>("url", "");
    
    if (url.empty()) {
        return AgentResult::error("URL is empty");
    }
    
    int timeout = request.get_param<int>("timeout_ms", timeout_ms_);
    
    return perform_request("HEAD", url, "", {}, timeout, user_agent_);
}

AgentResult WebSearchAgent::handle_search(const AgentRequest& request) {
    std::string query = request.query();
    std::string engine = request.get_param<std::string>("engine", "duckduckgo");
    
    if (query.empty()) {
        return AgentResult::error("Query is empty");
    }
    
    // URL-encoding запроса
    std::string encoded_query;
    for (char c : query) {
        if (c == ' ') {
            encoded_query += "+";
        } else if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
            encoded_query += c;
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned char>(c));
            encoded_query += hex;
        }
    }
    
    std::string url;
    if (engine == "duckduckgo") {
        url = "https://html.duckduckgo.com/html/?q=" + encoded_query;
    } else if (engine == "google") {
        url = "https://www.google.com/search?q=" + encoded_query;
    } else {
        return AgentResult::error("Unknown search engine: " + engine);
    }
    
    if (context_) {
        context_->info(name(), "Searching with " + engine + ": " + query);
    }
    
    return perform_request("GET", url, "", {}, timeout_ms_, user_agent_);
}

// ============================================================================
// Выполнение HTTP запроса
// ============================================================================

AgentResult WebSearchAgent::perform_request(const std::string& method,
                                             const std::string& url,
                                             const std::string& body,
                                             const std::vector<std::string>& headers,
                                             int timeout_ms,
                                             const std::string& user_agent) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return AgentResult::error("Failed to initialize CURL");
    }
    
    std::string response_body;
    std::string response_headers;
    long response_code = 0;
    
    auto start = std::chrono::steady_clock::now();
    
    // Настройка CURL
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, max_redirects_);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());
    
    // Заголовки для ответа
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);
    
    // Настройка заголовков запроса
    struct curl_slist* header_list = nullptr;
    for (const auto& header : headers) {
        header_list = curl_slist_append(header_list, header.c_str());
    }
    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }
    
    // Метод запроса
    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    } else if (method == "PUT") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    } else if (method == "DELETE") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    } else if (method == "HEAD") {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    }
    
    // Выполнение
    CURLcode res = curl_easy_perform(curl);
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - start).count();
    
    // Получение кода ответа
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    
    // Очистка
    if (header_list) {
        curl_slist_free_all(header_list);
    }
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        return AgentResult::error(std::string("CURL error: ") + curl_easy_strerror(res));
    }
    
    if (context_) {
        context_->debug(name(), method + " " + url + " -> " + 
                       std::to_string(response_code) + " (" + 
                       std::to_string(duration) + "ms)");
    }
    
    nlohmann::json result;
    result["url"] = url;
    result["method"] = method;
    result["status_code"] = static_cast<int>(response_code);
    result["body"] = response_body;
    result["headers"] = response_headers;
    result["duration_ms"] = duration;
    result["success"] = (response_code >= 200 && response_code < 300);
    
    return AgentResult::success(result);
}

} // namespace agents

// ============================================================================
// C-API экспорт
// ============================================================================

extern "C" {

AGENT_PLUGIN_EXPORT const char* plugin_get_name() {
    return "web_search_agent";
}

AGENT_PLUGIN_EXPORT const char* plugin_get_version() {
    return "1.0.0";
}

AGENT_PLUGIN_EXPORT const char* plugin_get_api_version() {
    return AGENT_PLUGIN_API_VERSION;
}

AGENT_PLUGIN_EXPORT agents::IAgent* plugin_create_agent() {
    return new agents::WebSearchAgent();
}

AGENT_PLUGIN_EXPORT void plugin_destroy_agent(agents::IAgent* agent) {
    delete agent;
}

AGENT_PLUGIN_EXPORT PluginExports* plugin_get_exports() {
    static PluginExports exports = {
        AGENT_PLUGIN_API_VERSION,
        plugin_get_name,
        plugin_get_version,
        plugin_get_api_version,
        reinterpret_cast<plugin_create_agent_fn>(plugin_create_agent),
        reinterpret_cast<plugin_destroy_agent_fn>(plugin_destroy_agent),
        nullptr,
        nullptr
    };
    return &exports;
}

} // extern "C"
