#include "sink.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "http.h"

namespace news_rewriter {

namespace {

// v1 HttpSink: отправка статьи JSON-POST'ом на endpoint. Параметры берёт из
// cfg.params: url (обязательный), api_key (необязательно, → Authorization:
// Bearer), timeout_seconds, max_retries (повторы при сбое, по умолчанию 0),
// retry_delay_ms (пауза между повторами, по умолчанию 1000). Storage не
// используется для записи — worker опирается на него только для дедупликации.
class HttpSink : public Sink {
public:
    HttpSink(const SinkConfig& cfg, const LogFn& log)
        : url_(cfg.params.get("url").as_string()),
          api_key_(cfg.params.get("api_key").as_string()),
          timeout_(static_cast<int>(
              cfg.params.get("timeout_seconds").as_int(20))),
          max_retries_(static_cast<int>(
              cfg.params.get("max_retries").as_int(0))),
          retry_delay_ms_(static_cast<int>(
              cfg.params.get("retry_delay_ms").as_int(1000))),
          log_(log) {}

    bool write(const Article& article) override {
        if (url_.empty()) {
            if (log_) log_("HttpSink: не задан URL (params.url)");
            return false;
        }
        if (!client_.init()) {
            if (log_) log_("HttpSink: libcurl недоступен");
            return false;
        }

        NetworkConfig nc;
        nc.timeout_seconds = timeout_;
        std::vector<std::string> headers;
        if (!api_key_.empty()) headers.push_back("Authorization: Bearer " + api_key_);

        const std::string payload = article_to_json(article).dump();
        int attempt = 0;
        for (;;) {
            const HttpResponse resp = client_.post(url_, payload, nc, headers);
            if (resp.ok && resp.status >= 200 && resp.status < 300) {
                if (log_) {
                    log_("HttpSink: отправлено " + article.url +
                         " (HTTP " + std::to_string(resp.status) + ")");
                }
                return true;
            }
            if (log_) {
                log_("HttpSink: отправка не удалась (HTTP " +
                     std::to_string(resp.status) + "): " +
                     (resp.error.empty() ? "нет данных" : resp.error));
            }
            if (attempt >= max_retries_) return false;
            if (retry_delay_ms_ > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms_));
            }
            attempt++;
        }
    }

    const char* name() const override { return "http"; }

private:
    HttpClient client_;
    std::string url_;
    std::string api_key_;
    int timeout_ = 20;
    int max_retries_ = 0;
    int retry_delay_ms_ = 1000;
    LogFn log_;
};

} // namespace

// Регистрируется в ll_plugin_init под "http" (см. plugin_main.cpp).
std::unique_ptr<Sink> make_http_sink(const SinkConfig& cfg, Storage&,
                                     const LogFn& log) {
    return std::make_unique<HttpSink>(cfg, log);
}

} // namespace news_rewriter
