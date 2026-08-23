#include "../../include/core/openrouter_http_client.h"
#include "../../include/core/logger.h"
#include <iostream>

namespace llama_gui {
namespace core {

OpenRouterHttpClient::~OpenRouterHttpClient() {
}

void OpenRouterHttpClient::set_base_url(const std::string& url) {
    base_url_ = url;
    if (!base_url_.empty() && base_url_.back() == '/') {
        base_url_.pop_back();
    }
}

void OpenRouterHttpClient::set_api_key(const std::string& api_key) {
    api_key_ = api_key;
}

void OpenRouterHttpClient::set_timeout(int timeout_ms) {
    timeout_ms_ = timeout_ms;
}

std::string OpenRouterHttpClient::build_url(const std::string& endpoint) {
    return base_url_ + "/" + endpoint;
}

std::vector<std::string> OpenRouterHttpClient::get_request_headers() const {
    std::vector<std::string> headers;

    headers.push_back("Content-Type: application/json");

    if (!api_key_.empty()) {
        headers.push_back("Authorization: Bearer " + api_key_);
    }

    headers.push_back("HTTP-Referer: https://github.com/llama-gui");
    headers.push_back("X-Title: llama-gui");

    return headers;
}

size_t OpenRouterHttpClient::write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    std::string* response = static_cast<std::string*>(userp);
    response->append(static_cast<char*>(contents), total_size);
    return total_size;
}

size_t OpenRouterHttpClient::stream_write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    auto* ctx = static_cast<StreamContext*>(userp);
    ctx->buffer.append(static_cast<char*>(contents), total_size);

    // Обрабатываем завершённые SSE-строки
    while (true) {
        size_t pos = ctx->buffer.find('\n');
        if (pos == std::string::npos) break;

        std::string line = ctx->buffer.substr(0, pos);
        ctx->buffer.erase(0, pos + 1);

        // Убираем \r
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // Пропускаем пустые строки и не-data строки (комментарии ": ..." и т.п.)
        if (line.empty()) continue;
        if (line.find("data: ") != 0) continue;

        std::string data = line.substr(6);
        if (data == "[DONE]") {
            ctx->done = true;
            return 0;  // Останавливаем передачу, curl вернёт CURLE_WRITE_ERROR
        }

        ctx->callback(data, false, false, false);
    }

    return total_size;
}

bool OpenRouterHttpClient::make_streaming_request(const std::string& endpoint, const std::string& body, const StreamCallback& callback, int connect_timeout_ms) {
    if (!callback) return false;

    std::string url = build_url(endpoint);

    CURL* curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("OpenRouter: Не удалось инициализировать CURL");
        callback("", true, true, false);
        return false;
    }

    struct curl_slist* headers = nullptr;
    auto header_list = get_request_headers();
    for (const auto& h : header_list) {
        headers = curl_slist_append(headers, h.c_str());
    }

    StreamContext ctx;
    ctx.callback = callback;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    // Стриминг: без общего таймаута - соединение живёт до [DONE] или закрытия сервером
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    std::cout << "[CloudClient] POST " << url << " (streaming)" << std::endl;

    CURLcode res = curl_easy_perform(curl);

    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    last_http_code_ = response_code;

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (ctx.done) {
        // Нормальное завершение после маркера [DONE]
        callback("", false, true, false);
        return true;
    }

    if (res != CURLE_OK && res != CURLE_WRITE_ERROR) {
        std::string error_msg = curl_easy_strerror(res);
        LOG_ERROR("CloudClient: Streaming request error: " + error_msg);
        std::cerr << "[CloudClient] CURL Error: " << error_msg << std::endl;
        std::cerr << "[CloudClient] URL: " << url << std::endl;
        // Транспортные ошибки (таймаут, нет соединения, DNS, сброс) транзиентные - ретраим
        callback("", true, true, true);
        return false;
    }

    if (response_code != 200) {
        // Не-SSE тело ошибки (JSON от API) накопилось в buffer
        std::cerr << "[CloudClient] HTTP Error: " << response_code << std::endl;
        std::cerr << "[CloudClient] Response: " << ctx.buffer.substr(0, 500) << std::endl;

        static const auto is_retryable_http_code = [](long code) {
            return code == 408 || code == 429 ||
                   code == 500 || code == 502 || code == 503 || code == 504;
        };

        callback(ctx.buffer, true, true, is_retryable_http_code(response_code));
        return false;
    }

    // Нормальное завершение без [DONE] (сервер закрыл соединение)
    callback("", false, true, false);
    return true;
}

std::string OpenRouterHttpClient::make_request(const std::string& endpoint, const std::string& body) {
    std::string response;
    std::string url = build_url(endpoint);

    CURL* curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("OpenRouter: Не удалось инициализировать CURL");
        return "";
    }

    struct curl_slist* headers = nullptr;
    auto header_list = get_request_headers();
    for (const auto& h : header_list) {
        headers = curl_slist_append(headers, h.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms_);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

    if (!body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());

        std::cout << "[CloudClient] POST " << url << std::endl;
        std::cout << "[CloudClient] Request body: " << body.substr(0, 200) << "..." << std::endl;
    }

    CURLcode res = curl_easy_perform(curl);

    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    last_http_code_ = response_code;

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::string error_msg = curl_easy_strerror(res);
        LOG_ERROR("CloudClient: Request error: " + error_msg);
        std::cerr << "[CloudClient] CURL Error: " << error_msg << std::endl;
        std::cerr << "[CloudClient] URL: " << url << std::endl;
        std::cerr << "[CloudClient] Response size: " << response.size() << " bytes" << std::endl;
        return "";
    }

    if (response_code != 200) {
        std::cerr << "[CloudClient] HTTP Error: " << response_code << std::endl;
        std::cerr << "[CloudClient] Response: " << response.substr(0, 500) << std::endl;
    }

    std::cout << "[CloudClient] Response size: " << response.size() << " bytes, HTTP " << response_code << std::endl;
    return response;
}

} // namespace core
} // namespace llama_gui
