#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "config.h"

namespace news_rewriter {

// Результат HTTP-запроса.
struct HttpResponse {
    bool ok = false;          // транспонировалось ли (сеть/таймаут/обрезка)
    int status = 0;           // HTTP-код (200, 404, ...)
    std::string body;
    std::string final_url;    // после редиректов
    std::string content_type; // заголовок Content-Type (вкл. charset)
    std::string error;
};

// HTTP-клиент на libcurl, загружаемом в рантайме через dlopen.
// Полностью изолирован от системы сборки приложения (никаких линк-зависимостей).
class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // Загружает libcurl и резолвит символы. Безопасно вызвать один раз.
    bool init();
    bool is_available() const { return available_; }

    HttpResponse get(const std::string& url, const NetworkConfig& cfg,
                      std::size_t max_bytes = 5 * 1024 * 1024);

    // GET с дополнительными заголовками (напр. Authorization), как в post.
    HttpResponse get(const std::string& url, const NetworkConfig& cfg,
                     const std::vector<std::string>& extra_headers,
                     std::size_t max_bytes = 5 * 1024 * 1024);

    // POST с JSON-телом; extra_headers (напр. Authorization) добавляются к
    // заголовкам из cfg.extra_headers. Используется HttpSink (этап 6).
    HttpResponse post(const std::string& url, const std::string& body,
                      const NetworkConfig& cfg,
                      const std::vector<std::string>& extra_headers = {},
                      std::size_t max_bytes = 5 * 1024 * 1024);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool available_ = false;
};

} // namespace news_rewriter
