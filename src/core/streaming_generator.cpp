#include "streaming_generator.h"
#include <iostream>
#include <sstream>
#include <chrono>

#ifdef USE_CURL
#include <curl/curl.h>
#endif

namespace llama_gui {
namespace core {

StreamingGenerator::StreamingGenerator() = default;
StreamingGenerator::~StreamingGenerator() = default;

static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string StreamingGenerator::generate_streaming(const std::string& prompt,
                                                   StreamCallback callback,
                                                   int max_tokens,
                                                   float temperature) {
    if (server_url_.empty()) {
        std::cerr << "[StreamingGenerator] No server URL configured" << std::endl;
        return "";
    }

    generating_ = true;
    auto start_time = std::chrono::steady_clock::now();

    std::string full_response;
    
#ifdef USE_CURL
    std::string url = server_url_;
    if (url.back() != '/') url += '/';
    url += "v1/chat/completions";

    // Build JSON request with streaming enabled
    std::string json_body = "{";
    json_body += "\"model\": \"gemma\",";
    json_body += "\"messages\": [{\"role\": \"user\", \"content\": \"";

    // Escape JSON string
    for (char c : prompt) {
        switch (c) {
            case '"':  json_body += "\\\""; break;
            case '\\': json_body += "\\\\"; break;
            case '\n': json_body += "\\n"; break;
            case '\r': json_body += "\\r"; break;
            case '\t': json_body += "\\t"; break;
            default:   json_body += c; break;
        }
    }

    json_body += "\"}],";
    json_body += "\"stream\": true,";
    json_body += "\"temperature\": " + std::to_string(temperature) + ",";
    json_body += "\"max_tokens\": " + std::to_string(max_tokens);
    json_body += "}";

    CURL* curl = curl_easy_init();
    if (!curl) {
        generating_ = false;
        return "";
    }

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 120000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
        std::cerr << "[StreamingGenerator] Request failed: "
                  << (res != CURLE_OK ? curl_easy_strerror(res) : "HTTP " + std::to_string(http_code))
                  << std::endl;
        generating_ = false;
        return "";
    }

    // Parse SSE response
    // Format: data: {"choices":[{"delta":{"content":"token"}}]}\n\n
    std::istringstream stream(response);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.substr(0, 6) == "data: ") {
            std::string data = line.substr(6);

            // Check for [DONE]
            if (data == "[DONE]") {
                break;
            }

            // Try to extract content from delta
            size_t content_pos = data.find("\"content\":\"");
            if (content_pos != std::string::npos) {
                size_t start = content_pos + 11;
                size_t end = data.find("\"", start);
                if (end != std::string::npos) {
                    std::string token = data.substr(start, end - start);
                    if (!token.empty()) {
                        full_response += token;
                        if (callback) {
                            callback(token, false);
                        }
                    }
                }
            }
        }
    }

    // Send final callback
    if (callback) {
        callback("", true);
    }

#endif

    generating_ = false;

    // Calculate stats
    auto end_time = std::chrono::steady_clock::now();
    last_stats_.generation_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    last_stats_.total_tokens = static_cast<int>(full_response.size() / 4); // Approximate
    if (last_stats_.generation_time_ms > 0) {
        last_stats_.tokens_per_second = last_stats_.total_tokens * 1000.0 / last_stats_.generation_time_ms;
    }

    return full_response;
}

std::string StreamingGenerator::generate_sync(const std::string& prompt, int max_tokens, float temperature) {
    // Non-streaming version for fallback
    return generate_streaming(prompt, nullptr, max_tokens, temperature);
}

bool StreamingGenerator::is_server_available() const {
    if (server_url_.empty()) return false;

#ifdef USE_CURL
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string url = server_url_ + "/health";
    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK && response.find("\"ok\"") != std::string::npos);
#else
    return false;
#endif
}

} // namespace core
} // namespace llama_gui
