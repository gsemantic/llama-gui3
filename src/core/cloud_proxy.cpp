#include "core/cloud_proxy.h"
#include "core/rag_manager.h"
#include "core/env_manager.h"
#include "core/logger.h"
#include "core/net_utils.h"
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <atomic>
#include <csignal>
#include <thread>
#include <mutex>
#include <vector>
#include <map>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace llama_gui {
namespace core {

namespace {

std::atomic<bool> g_proxy_stop{false};

void proxy_signal_handler(int) {
    g_proxy_stop.store(true);
}

std::string to_lower_str(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers; // ключи в нижнем регистре
    std::string body;
};

// Чтение полного HTTP-запроса (заголовки + тело по Content-Length)
bool read_request(int fd, HttpRequest& out) {
    std::string raw;
    char buf[16384];
    struct timeval tv{30, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    size_t header_end = std::string::npos;
    size_t content_length = 0;
    bool have_cl = false;

    while (true) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        raw.append(buf, static_cast<size_t>(n));

        if (header_end == std::string::npos) {
            header_end = raw.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                std::string head = raw.substr(0, header_end);
                size_t eol = head.find("\r\n");
                if (eol == std::string::npos) return false;
                std::string request_line = head.substr(0, eol);

                size_t sp = request_line.find(' ');
                if (sp == std::string::npos) return false;
                out.method = request_line.substr(0, sp);
                size_t sp2 = request_line.find(' ', sp + 1);
                out.path = request_line.substr(sp + 1,
                             sp2 == std::string::npos ? std::string::npos : sp2 - sp - 1);

                size_t pos = eol + 2;
                while (pos < head.size()) {
                    size_t e = head.find("\r\n", pos);
                    if (e == std::string::npos) e = head.size();
                    std::string line = head.substr(pos, e - pos);
                    pos = e + 2;
                    size_t colon = line.find(':');
                    if (colon != std::string::npos) {
                        std::string key = to_lower_str(line.substr(0, colon));
                        std::string val = line.substr(colon + 1);
                        size_t b = val.find_first_not_of(" \t");
                        size_t e2 = val.find_last_not_of(" \t");
                        val = (b == std::string::npos) ? "" : val.substr(b, e2 - b + 1);
                        out.headers[key] = val;
                        if (key == "content-length") {
                            content_length = static_cast<size_t>(std::strtoull(val.c_str(), nullptr, 10));
                            have_cl = true;
                        }
                    }
                }
            }
        }

        if (header_end != std::string::npos) {
            size_t body_start = header_end + 4;
            size_t have_body = raw.size() - body_start;
            if (have_cl) {
                if (have_body >= content_length) {
                    out.body = raw.substr(body_start, content_length);
                    return true;
                }
            } else {
                out.body = raw.substr(body_start);
                return true;
            }
        }
        if (raw.size() > 128ull * 1024ull * 1024ull) return false;
    }
}

void send_all(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

void send_response(int fd, int status, const std::string& content_type,
                   const std::string& body, bool stream = false) {
    std::string status_text = (status == 200) ? "OK"
                            : (status == 400) ? "Bad Request"
                            : (status == 404) ? "Not Found"
                            : (status == 502) ? "Bad Gateway" : "Error";
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + status_text + "\r\n";
    head += "Content-Type: " + content_type + "\r\n";
    if (stream) {
        head += "Cache-Control: no-cache\r\n";
        head += "X-Accel-Buffering: no\r\n";
    } else {
        head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    head += "Connection: close\r\n\r\n";
    send_all(fd, head + body);
}

// ---- Пересылка через libcurl ----

struct CollectCtx { std::string data; };
struct StreamCtx { int fd; bool saw_done = false; };

size_t collect_write_cb(void* ptr, size_t size, size_t nmemb, void* userp) {
    auto* ctx = static_cast<CollectCtx*>(userp);
    ctx->data.append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

size_t stream_write_cb(void* ptr, size_t size, size_t nmemb, void* userp) {
    auto* ctx = static_cast<StreamCtx*>(userp);
    size_t total = size * nmemb;
    const char* data = static_cast<const char*>(ptr);
    std::string chunk(data, total);

    // reasoning-модели: если delta.content пуст, а delta.reasoning_content есть,
    // подставляем его в content, чтобы обычные клиенты видели текст.
    if (chunk.find("\"reasoning_content\"") != std::string::npos &&
        chunk.find("data:") != std::string::npos) {
        size_t start = 0;
        while (true) {
            size_t dl = chunk.find("data:", start);
            if (dl == std::string::npos) break;
            size_t line_end = chunk.find('\n', dl);
            std::string line = chunk.substr(dl, line_end == std::string::npos ? std::string::npos : line_end - dl);
            std::string payload = line.substr(5);
            size_t trimmed = payload.find_first_not_of(" \t");
            if (trimmed != std::string::npos) payload = payload.substr(trimmed);
            if (payload == "[DONE]") break;
            try {
                nlohmann::json j = nlohmann::json::parse(payload);
                if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
                    auto& choice = j["choices"][0];
                    bool has_content = choice.contains("delta") && choice["delta"].is_object() &&
                        choice["delta"].contains("content") && choice["delta"]["content"].is_string() &&
                        !choice["delta"]["content"].get<std::string>().empty();
                    bool has_reasoning = choice.contains("delta") && choice["delta"].is_object() &&
                        choice["delta"].contains("reasoning_content") &&
                        choice["delta"]["reasoning_content"].is_string();
                    if (!has_content && has_reasoning) {
                        choice["delta"]["content"] = choice["delta"]["reasoning_content"];
                        std::string fixed = "data: " + j.dump();
                        if (line_end != std::string::npos) fixed += "\n";
                        chunk.replace(dl, (line_end == std::string::npos ? chunk.size() : line_end + 1) - dl, fixed);
                    }
                }
            } catch (...) {
                // невалидный чанк — оставляем как есть
            }
            if (line_end == std::string::npos) break;
            start = line_end + 1;
        }
    }

    size_t sent = 0;
    while (sent < chunk.size()) {
        ssize_t n = send(ctx->fd, chunk.data() + sent, chunk.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return 0; // клиент отключился — прерываем
        sent += static_cast<size_t>(n);
    }
    if (chunk.find("[DONE]") != std::string::npos) ctx->saw_done = true;
    return total;
}

// Обычный запрос (не-стриминг). Возвращает HTTP-статус апстрима (0 = ошибка curl)
int forward_collect(const std::string& url, const std::vector<std::string>& headers,
                    const std::string& body, std::string& out_body) {
    CURL* curl = curl_easy_init();
    if (!curl) return 0;
    CollectCtx ctx;
    struct curl_slist* hdrs = nullptr;
    for (const auto& h : headers) hdrs = curl_slist_append(hdrs, h.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    out_body = ctx.data;
    return (res == CURLE_OK) ? static_cast<int>(code) : 0;
}

// Стриминговый запрос: SSE пишется напрямую клиенту
int forward_stream(const std::string& url, const std::vector<std::string>& headers,
                   const std::string& body, int client_fd) {
    CURL* curl = curl_easy_init();
    if (!curl) return 0;
    StreamCtx ctx{client_fd, false};
    struct curl_slist* hdrs = nullptr;
    for (const auto& h : headers) hdrs = curl_slist_append(hdrs, h.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 30000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (res == CURLE_OK && code == 200 && !ctx.saw_done) {
        const char* done = "data: [DONE]\n\n";
        send(client_fd, done, 14, MSG_NOSIGNAL);
    }
    return (res == CURLE_OK) ? static_cast<int>(code) : 0;
}

// ---- URL апстрима (аналог OpenRouterHttpClient::build_url) ----
std::string build_upstream_url(const std::string& endpoint_url, const std::string& suffix) {
    std::string base = endpoint_url;
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    return base + "/" + suffix;
}

// GLM и другие reasoning-модели кладут вывод в reasoning_content, а content оставляют
// пустым. Обычные OpenAI-клиенты (qwen cli и пр.) читают только content, поэтому
// подставляем reasoning_content в content, если тот пуст.
std::string normalize_reasoning_response(const std::string& out_body) {
    if (out_body.empty()) return out_body;
    try {
        nlohmann::json j = nlohmann::json::parse(out_body);
        if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
            auto& choice = j["choices"][0];
            if (choice.contains("message") && choice["message"].is_object()) {
                auto& msg = choice["message"];
                bool content_empty = !msg.contains("content") || msg["content"].is_null() ||
                    (msg["content"].is_string() && msg["content"].get<std::string>().empty());
                if (content_empty && msg.contains("reasoning_content") &&
                    msg["reasoning_content"].is_string() &&
                    !msg["reasoning_content"].get<std::string>().empty()) {
                    msg["content"] = msg["reasoning_content"];
                }
            }
        }
        return j.dump();
    } catch (...) {
        return out_body;
    }
}

// ---- RAG: дополнение последнего сообщения пользователя контекстом ----
bool apply_rag(RagManager* rag, const RagSettings& rs, nlohmann::json& req) {
    if (!rag) return false;
    if (!req.contains("messages") || !req["messages"].is_array()) return false;
    auto& msgs = req["messages"];

    int last_user = -1;
    for (int i = static_cast<int>(msgs.size()) - 1; i >= 0; --i) {
        auto& m = msgs[static_cast<size_t>(i)];
        if (m.contains("role") && m["role"] == "user") { last_user = i; break; }
    }
    if (last_user < 0) return false;

    // content может быть массивом (мультимодальные части) — извлекаем текстовые части
    auto& content_val = msgs[static_cast<size_t>(last_user)]["content"];
    std::string query;
    if (content_val.is_string()) {
        query = content_val.get<std::string>();
    } else if (content_val.is_array()) {
        for (auto& part : content_val) {
            if (part.is_string()) {
                query += part.get<std::string>();
            } else if (part.is_object() && part.contains("text") && part["text"].is_string()) {
                query += part["text"].get<std::string>();
            }
        }
    }
    if (query.empty()) return false;

    int k = (rs.search_k > 0) ? rs.search_k : 5;
    std::vector<RagChunk> results;
    if (rs.enable_hybrid_search) {
        results = rag->search_hybrid(query, k);
    } else {
        results = rag->search_external_documents(query, k);
    }
    if (results.empty()) return false;

    std::string augmented = rag->build_rag_prompt(query, results, /*is_cloud_model=*/true);
    if (augmented.empty() || augmented == query) return false;

    msgs[static_cast<size_t>(last_user)]["content"] = augmented;

    bool has_system = false;
    for (auto& m : msgs) {
        if (m.contains("role") && m["role"] == "system") { has_system = true; break; }
    }
    if (!has_system) {
        nlohmann::json sys;
        sys["role"] = "system";
        sys["content"] = "Ты - полезный ассистент с доступом к внешним документам. "
                         "Используй предоставленный контекст для ответа на вопрос. "
                         "Отвечай кратко и по делу.";
        msgs.insert(msgs.begin(), sys);
    }
    return true;
}

// ---- Дефолты из настроек приложения для параметров, не указанных клиентом ----
void apply_defaults(Settings& settings, nlohmann::json& req) {
    const auto& chat = settings.chat();
    const auto& cp = settings.cloud_provider();

    bool has_model = req.contains("model") && !req["model"].is_null() &&
                     req["model"].is_string() && !req["model"].get<std::string>().empty();
    if (has_model) {
        // qwen cli и подобные клиенты шлют плейсхолдер "local" — заменяем на модель провайдера
        std::string m = req["model"].get<std::string>();
        if (m == "local" || m == "default" || m == "gpt-4o" || m == "gpt-4o-mini") {
            req["model"] = cp.model_id;
        }
    } else {
        req["model"] = cp.model_id;
    }
    const auto normalize_param = [](nlohmann::json& j) {
        if (j.is_number()) {
            j = std::round(j.get<double>() * 100.0) / 100.0;
        }
    };
    if (!req.contains("temperature")) req["temperature"] = chat.temperature;
    normalize_param(req["temperature"]);
    if (!req.contains("top_p")) req["top_p"] = chat.top_p;
    normalize_param(req["top_p"]);
    if (!req.contains("max_tokens") || req["max_tokens"].is_null()) {
        if (cp.max_output_tokens > 0) req["max_tokens"] = cp.max_output_tokens;
        else req.erase("max_tokens");
    }
    if (!req.contains("presence_penalty") && chat.presence_penalty != 0.0f) {
        req["presence_penalty"] = chat.presence_penalty;
    }
    normalize_param(req["presence_penalty"]);
    if (!req.contains("frequency_penalty") && chat.frequency_penalty != 0.0f) {
        req["frequency_penalty"] = chat.frequency_penalty;
    }
    normalize_param(req["frequency_penalty"]);

    // Режим размышлений/thinking.
    if (!req.contains("thinking")) {
        if (cp.reasoning_enabled) {
            nlohmann::json thinking = nlohmann::json::object({{"type", "enabled"}});
            if (cp.reasoning_budget > 0) {
                thinking["budget_tokens"] = cp.reasoning_budget;
            }
            req["thinking"] = thinking;
        } else if (req.contains("model") && req["model"].is_string()) {
            const std::string m = req["model"].get<std::string>();
            // Модели, которые по умолчанию включают thinking и тратят лимит на
            // reasoning_content, оставляя content пустым (GLM, OpenCode hy3-free и
            // т.п. через zen-endpoint). Для них явно отключаем размышления, чтобы
            // ответ попадал в content, а не в reasoning_content.
            const bool thinking_by_default =
                m.find("glm") != std::string::npos ||
                m.find("hy3") != std::string::npos ||
                cp.endpoint_url.find("opencode.ai") != std::string::npos;
            if (thinking_by_default) {
                req["thinking"] = nlohmann::json::object({{"type", "disabled"}});
            }
        }
    }

    // Системный промпт приложения — только если клиент не прислал свой
    bool has_system = false;
    if (req.contains("messages") && req["messages"].is_array()) {
        for (auto& m : req["messages"]) {
            if (m.contains("role") && m["role"] == "system") { has_system = true; break; }
        }
    }
    if (!has_system && !chat.default_system_prompt.empty() &&
        req.contains("messages") && req["messages"].is_array()) {
        nlohmann::json sys;
        sys["role"] = "system";
        sys["content"] = chat.default_system_prompt;
        req["messages"].insert(req["messages"].begin(), sys);
    }
}

// ---- Контекст прокси: живые настройки + ленивый RAG ----
struct ProxyContext {
    Settings& settings;
    CloudProxyOptions opts;   // копия опций (переопределения CLI)
    std::mutex rag_mutex;
    std::unique_ptr<RagManager> rag;
    bool rag_init_tried = false;

    // Актуальный endpoint провайдера: приоритет override CLI > живые настройки
    std::string endpoint_url() const {
        return opts.endpoint_url.empty() ? settings.cloud_provider().endpoint_url
                                         : opts.endpoint_url;
    }

    // Актуальный API-ключ: override CLI > ключ из .env для текущего провайдера
    std::string api_key() const {
        if (!opts.api_key.empty()) return opts.api_key;
        const auto& cp = settings.cloud_provider();
        std::string key_name = EnvManager::cloud_provider_api_key_name(cp.provider_name,
                                                                       cp.endpoint_url);
        return EnvManager::read_key(key_name, settings.get_profiles_directory());
    }

    // Инициализация RAG из живых настроек (вызывать под rag_mutex)
    RagManager* get_rag_locked() {
        if (rag) return rag.get();
        if (rag_init_tried) return nullptr;
        rag_init_tried = true;
        if (settings.rag().enable_rag && !settings.get_embedding_model_path().empty()) {
            auto r = std::make_unique<RagManager>(settings.get_embedding_model_path());
            if (r->initialize_indexes()) {
                r->update_from_settings(settings.rag());
                // НЕ переопределяем профили здесь: конструктор RagManager уже
                // инициализировал менеджер профилей через ~/.llama-gui/rag_profiles/.
                // get_profiles_directory() возвращает относительный "profiles",
                // где индексов нет — RAG вернул бы 0 результатов.
                r->load_index_for_current_profile();
                LOG_INFO("Cloud proxy: RAG инициализирован (локальный поиск)");
                rag = std::move(r);
            } else {
                LOG_WARNING("Cloud proxy: RAG недоступен (нет эмбеддинга/индекса), работаю без контекста");
            }
        }
        return rag.get();
    }
};

// ---- Обработка одного соединения ----
void handle_connection(int fd, ProxyContext& ctx) {
    Settings& settings = ctx.settings;
    HttpRequest req;
    if (!read_request(fd, req)) { close(fd); return; }

    if (req.method == "GET" && (req.path == "/health" || req.path == "/healthz")) {
        send_response(fd, 200, "application/json", "{\"status\":\"ok\"}");
        close(fd);
        return;
    }

    if (req.method == "GET" && req.path == "/") {
        send_response(fd, 200, "application/json",
                      "{\"service\":\"llama-gui cloud proxy\",\"openai\":\"/v1\"}");
        close(fd);
        return;
    }

    if (req.method == "GET" && req.path == "/v1/models") {
        const auto& cp = settings.cloud_provider();
        std::string mid = cp.model_id.empty() ? "cloud-model" : cp.model_id;
        nlohmann::json m;
        m["id"] = mid;
        m["object"] = "model";
        m["created"] = 0;
        m["owned_by"] = "cloud-proxy";
        m["type"] = "model";
        nlohmann::json resp;
        resp["object"] = "list";
        resp["data"] = nlohmann::json::array({m});
        send_response(fd, 200, "application/json", resp.dump());
        close(fd);
        return;
    }

    bool is_chat = (req.path == "/v1/chat/completions" || req.path == "/chat/completions") && req.method == "POST";
    if (is_chat) {
        nlohmann::json in;
        try {
            in = nlohmann::json::parse(req.body);
        } catch (...) {
            send_response(fd, 400, "application/json",
                          "{\"error\":{\"message\":\"invalid json body\"}}");
            close(fd);
            return;
        }

        bool want_stream = in.value("stream", false);

        // RAG (только если включён и есть индекс) — ленивая инициализация
        bool rag_applied = false;
        {
            std::lock_guard<std::mutex> lock(ctx.rag_mutex);
            RagManager* rag = ctx.get_rag_locked();
            if (rag && settings.rag().enable_rag) {
                if (apply_rag(rag, settings.rag(), in)) {
                    std::cout << "[CloudProxy] RAG контекст применён" << std::endl;
                    rag_applied = true;
                }
            }
        }

        apply_defaults(settings, in);
        std::string body = in.dump();

        std::string out_model = in.contains("model") && in["model"].is_string() ? in["model"].get<std::string>() : "";
        std::string out_temp = in.contains("temperature") ? in["temperature"].dump() : "none";
        std::string out_max = in.contains("max_tokens") ? in["max_tokens"].dump() : "none";
        std::cout << "[CloudProxy] outgoing: model=" << out_model
                  << " temperature=" << out_temp
                  << " max_tokens=" << out_max
                  << " stream=" << (want_stream ? "yes" : "no")
                  << (rag_applied ? " RAG=yes" : "") << std::endl;

        std::vector<std::string> headers;
        headers.push_back("Content-Type: application/json");
        std::string api_key = ctx.api_key();
        if (!api_key.empty()) headers.push_back("Authorization: Bearer " + api_key);
        headers.push_back("HTTP-Referer: https://github.com/llama-gui");
        headers.push_back("X-Title: llama-gui-cloud-proxy");

        std::string url = build_upstream_url(ctx.endpoint_url(), "chat/completions");
        std::cout << "[CloudProxy] -> " << url << (want_stream ? " (stream)" : "") << std::endl;

        if (want_stream) {
            send_response(fd, 200, "text/event-stream", "", /*stream=*/true);
            forward_stream(url, headers, body, fd);
        } else {
            std::string out_body;
            int code = forward_collect(url, headers, body, out_body);
            if (code <= 0) {
                send_response(fd, 502, "application/json",
                              "{\"error\":{\"message\":\"upstream request failed\"}}");
            } else {
                if (code == 200) out_body = normalize_reasoning_response(out_body);
                send_response(fd, code, "application/json", out_body);
            }
        }
        close(fd);
        return;
    }

    if ((req.path == "/v1/embeddings" || req.path == "/embeddings") && req.method == "POST") {
        std::vector<std::string> headers;
        headers.push_back("Content-Type: application/json");
        std::string api_key = ctx.api_key();
        if (!api_key.empty()) headers.push_back("Authorization: Bearer " + api_key);
        headers.push_back("HTTP-Referer: https://github.com/llama-gui");
        headers.push_back("X-Title: llama-gui-cloud-proxy");

        std::string out_body;
        int code = forward_collect(build_upstream_url(ctx.endpoint_url(), "embeddings"),
                                   headers, req.body, out_body);
        if (code <= 0) {
            send_response(fd, 502, "application/json",
                          "{\"error\":{\"message\":\"upstream failed\"}}");
        } else {
            send_response(fd, code, "application/json", out_body);
        }
        close(fd);
        return;
    }

    send_response(fd, 404, "application/json", "{\"error\":{\"message\":\"not found\"}}");
    close(fd);
}

} // namespace

// Внутренний accept-цикл прокси. Останавливается по stop_flag.
static int run_proxy_accept_loop(Settings& settings, CloudProxyOptions opts,
                                 std::atomic<bool>& stop_flag) {
    const auto& cp = settings.cloud_provider();
    std::string endpoint_url = opts.endpoint_url.empty() ? cp.endpoint_url : opts.endpoint_url;
    if (endpoint_url.empty()) {
        LOG_ERROR("Cloud proxy: не задан endpoint_url облачного провайдера (настройки Cloud в профиле)");
        return 1;
    }

    std::string host = opts.host.empty() ? settings.server_runtime().host : opts.host;
    if (host.empty()) host = "127.0.0.1";
    int port = opts.port > 0 ? opts.port : settings.server_runtime().port;
    if (port <= 0) port = 8081;

    if (opts.auto_port || is_port_in_use(port, host)) {
        int free_port = find_free_port(port, host);
        if (free_port < 0) {
            LOG_ERROR("Cloud proxy: не удалось найти свободный порт");
            return 1;
        }
        if (free_port != port) {
            std::cout << "Порт " << port << " занят, использую свободный порт " << free_port << std::endl;
            port = free_port;
        }
    }

    std::string api_key;
    if (!opts.api_key.empty()) {
        api_key = opts.api_key;
    } else {
        std::string key_name = EnvManager::cloud_provider_api_key_name(cp.provider_name,
                                                                       cp.endpoint_url);
        api_key = EnvManager::read_key(key_name, settings.get_profiles_directory());
    }

    ProxyContext ctx{settings, opts};
    RagManager* rag = nullptr;
    {
        std::lock_guard<std::mutex> lock(ctx.rag_mutex);
        rag = ctx.get_rag_locked();
    }

    if (opts.manage_curl) curl_global_init(CURL_GLOBAL_DEFAULT);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        LOG_ERROR("Cloud proxy: socket() failed");
        return 1;
    }
    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    if (bind(lfd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("Cloud proxy: bind failed: " + host + ":" + std::to_string(port));
        close(lfd);
        if (opts.manage_curl) curl_global_cleanup();
        return 1;
    }
    if (listen(lfd, 32) < 0) {
        LOG_ERROR("Cloud proxy: listen failed");
        close(lfd);
        if (opts.manage_curl) curl_global_cleanup();
        return 1;
    }

    std::cout << "======================================================" << std::endl;
    std::cout << "  ОБЛАЧНЫЙ ПРОКСИ (cloud proxy)" << std::endl;
    std::cout << "  Провайдер: " << cp.provider_name << std::endl;
    std::cout << "  Модель:    " << (cp.model_id.empty() ? "(из запроса клиента)" : cp.model_id) << std::endl;
    std::cout << "  Upstream:  " << endpoint_url << std::endl;
    std::cout << "  API key:   " << (api_key.empty() ? "НЕТ (провайдер может отклонить)" : api_key.substr(0, 6) + "...") << std::endl;
    std::cout << "  RAG:       " << (rag ? "включён (локальный поиск)" : "выключен") << std::endl;
    std::cout << "  Слушаю:    " << host << ":" << port << std::endl;
    std::cout << "  Endpoint:  http://" << host << ":" << port << "/v1" << std::endl;
    if (opts.manage_curl) {
        std::cout << "  Ctrl+C / SIGTERM - остановка" << std::endl;
    } else {
        std::cout << "  Останавливается вместе с приложением (GUI-режим)" << std::endl;
    }
    std::cout << "======================================================" << std::endl;

    std::vector<std::thread> workers;

    while (!stop_flag.load()) {
        struct pollfd pfd{lfd, POLLIN, 0};
        int pr = poll(&pfd, 1, 200);
        if (pr <= 0) continue;
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) continue;

        workers.emplace_back([cfd, &ctx]() {
            try {
                handle_connection(cfd, ctx);
            } catch (const nlohmann::json::exception& e) {
                std::cerr << "[CloudProxy] JSON error в обработке соединения: " << e.what() << std::endl;
                close(cfd);
            } catch (const std::exception& e) {
                std::cerr << "[CloudProxy] Ошибка в обработке соединения: " << e.what() << std::endl;
                close(cfd);
            } catch (...) {
                std::cerr << "[CloudProxy] Неизвестная ошибка в обработке соединения" << std::endl;
                close(cfd);
            }
        });
    }

    close(lfd);
    std::cout << "Остановка cloud proxy..." << std::endl;
    for (auto& t : workers) {
        if (t.joinable()) t.detach();
    }
    if (opts.manage_curl) curl_global_cleanup();
    return 0;
}

int run_cloud_proxy(Settings& settings, const CloudProxyOptions& opts) {
    CloudProxyOptions o = opts;
    g_proxy_stop.store(false);
    if (o.manage_curl) {
        std::signal(SIGINT, proxy_signal_handler);
        std::signal(SIGTERM, proxy_signal_handler);
    }
    return run_proxy_accept_loop(settings, o, g_proxy_stop);
}

std::thread start_cloud_proxy_in_thread(Settings& settings, const CloudProxyOptions& opts,
                                        std::atomic<bool>& stop_flag) {
    CloudProxyOptions o = opts;
    o.manage_curl = false; // GUI уже инициализировал curl
    return std::thread([&settings, o, &stop_flag]() {
        run_proxy_accept_loop(settings, o, stop_flag);
    });
}

} // namespace core
} // namespace llama_gui
