#include "http.h"

#include <dlfcn.h>
#include <cstring>
#include <string>
#include <vector>

namespace news_rewriter {

// ============================================================================
// Биндинги libcurl (минимальные типы и константы, ABI-стабильные)
// ============================================================================

namespace {

typedef void CURL;
typedef int CURLcode;
typedef int CURLoption;
typedef int CURLINFO;
typedef struct curl_slist curl_slist;

// CURLOPT_* (значения фиксированы в ABI curl)
const CURLoption kOptUrl = 10002;
const CURLoption kOptUserAgent = 10018;
const CURLoption kOptFollowLocation = 52;
const CURLoption kOptMaxRedirs = 68;
const CURLoption kOptTimeout = 13;
const CURLoption kOptConnectTimeout = 78;
const CURLoption kOptNoSignal = 99;
const CURLoption kOptWriteFunction = 20011;
const CURLoption kOptWriteData = 10001;
const CURLoption kOptProxy = 10004;
const CURLoption kOptHttpHeader = 10023;

// CURLINFO_* (код = тип-маска | номер)
const CURLINFO kInfoResponseCode = 0x200000 + 2;  // CURLINFO_RESPONSE_CODE
const CURLINFO kInfoEffectiveUrl = 0x100000 + 1;  // CURLINFO_EFFECTIVE_URL

struct CurlBind {
    void* handle = nullptr;
    bool loaded = false;

    CURLcode (*curl_global_init)(long) = nullptr;
    void (*curl_global_cleanup)(void) = nullptr;
    CURL* (*curl_easy_init)(void) = nullptr;
    void (*curl_easy_cleanup)(CURL*) = nullptr;
    CURLcode (*curl_easy_setopt)(CURL*, CURLoption, ...) = nullptr;
    CURLcode (*curl_easy_perform)(CURL*) = nullptr;
    CURLcode (*curl_easy_getinfo)(CURL*, CURLINFO, ...) = nullptr;
    const char* (*curl_easy_strerror)(CURLcode) = nullptr;
    curl_slist* (*curl_slist_append)(curl_slist*, const char*) = nullptr;
    void (*curl_slist_free_all)(curl_slist*) = nullptr;

    ~CurlBind() {
        if (handle) dlclose(handle);
    }

    bool load() {
        if (loaded) return true;
        const char* names[] = {"libcurl.so.4", "libcurl.so", nullptr};
        for (int i = 0; names[i]; i++) {
            handle = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);
            if (handle) break;
        }
        if (!handle) return false;

#define RESOLVE(fn) \
        fn = reinterpret_cast<decltype(fn)>(dlsym(handle, #fn)); \
        if (!fn) return false;

        RESOLVE(curl_global_init);
        RESOLVE(curl_global_cleanup);
        RESOLVE(curl_easy_init);
        RESOLVE(curl_easy_cleanup);
        RESOLVE(curl_easy_setopt);
        RESOLVE(curl_easy_perform);
        RESOLVE(curl_easy_getinfo);
        RESOLVE(curl_easy_strerror);
        RESOLVE(curl_slist_append);
        RESOLVE(curl_slist_free_all);
#undef RESOLVE

        loaded = true;
        return true;
    }
};

struct WriteCtx {
    std::string* body = nullptr;
    std::size_t max_bytes = 0;
    bool overflow = false;
};

// Колбэк записи curl: 0 = прерывание (лимит размера).
std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* ctx = static_cast<WriteCtx*>(userdata);
    const std::size_t chunk = size * nmemb;
    if (ctx->body->size() + chunk > ctx->max_bytes) {
        ctx->overflow = true;
        return 0;
    }
    ctx->body->append(ptr, chunk);
    return chunk;
}

} // namespace

struct HttpClient::Impl {
    CurlBind curl;
};

HttpClient::HttpClient() : impl_(std::make_unique<Impl>()) {}

HttpClient::~HttpClient() = default;

bool HttpClient::init() {
    if (available_) return true;
    if (!impl_->curl.load()) return false;
    impl_->curl.curl_global_init(0x0003 /*CURL_GLOBAL_ALL*/);
    available_ = true;
    return true;
}

HttpResponse HttpClient::get(const std::string& url, const NetworkConfig& cfg,
                             std::size_t max_bytes) {
    HttpResponse resp;
    if (!available_ || !impl_->curl.loaded) {
        resp.error = "libcurl недоступен (dlopen не удался)";
        return resp;
    }

    CurlBind& b = impl_->curl;
    CURL* easy = b.curl_easy_init();
    if (!easy) {
        resp.error = "curl_easy_init не удался";
        return resp;
    }

    WriteCtx ctx{&resp.body, max_bytes, false};

    b.curl_easy_setopt(easy, kOptUrl, url.c_str());
    b.curl_easy_setopt(easy, kOptTimeout, static_cast<long>(cfg.timeout_seconds));
    b.curl_easy_setopt(easy, kOptConnectTimeout, static_cast<long>(cfg.timeout_seconds));
    b.curl_easy_setopt(easy, kOptFollowLocation, 1L);
    b.curl_easy_setopt(easy, kOptMaxRedirs, 10L);
    b.curl_easy_setopt(easy, kOptNoSignal, 1L);
    b.curl_easy_setopt(easy, kOptWriteFunction, reinterpret_cast<void*>(write_cb));
    b.curl_easy_setopt(easy, kOptWriteData, &ctx);
    if (!cfg.user_agent.empty()) {
        b.curl_easy_setopt(easy, kOptUserAgent, cfg.user_agent.c_str());
    }
    if (!cfg.proxy.empty()) {
        b.curl_easy_setopt(easy, kOptProxy, cfg.proxy.c_str());
    }

    curl_slist* headers = nullptr;
    if (!cfg.extra_headers.empty()) {
        std::string h = cfg.extra_headers;
        std::size_t start = 0;
        while (start <= h.size()) {
            std::size_t nl = h.find('\n', start);
            const std::string line = h.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            if (!line.empty()) headers = b.curl_slist_append(headers, line.c_str());
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        if (headers) b.curl_easy_setopt(easy, kOptHttpHeader, headers);
    }

    const CURLcode rc = b.curl_easy_perform(easy);
    if (rc != 0) {
        resp.error = b.curl_easy_strerror ? b.curl_easy_strerror(rc) : "curl error";
        if (ctx.overflow) resp.error = "ответ превышает лимит размера";
    } else {
        resp.ok = true;
        long code = 0;
        b.curl_easy_getinfo(easy, kInfoResponseCode, &code);
        resp.status = static_cast<int>(code);
        char* eff = nullptr;
        if (b.curl_easy_getinfo(easy, kInfoEffectiveUrl, &eff) == 0 && eff) {
            resp.final_url = eff;
        }
    }

    if (headers) b.curl_slist_free_all(headers);
    b.curl_easy_cleanup(easy);
    return resp;
}

} // namespace news_rewriter
