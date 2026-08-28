#include "url_norm.h"

#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>

namespace news_rewriter {

namespace {

// ---------------------------------------------------------------------------
// IDN -> ASCII (punycode). Основной путь — libidn2 (dlopen, как и libcurl в
// http.cpp), она делает полноценный IDNA2008. Если libidn2 недоступна —
// встроенный кодировщик punycode (покрывает кириллицу и прочий Unicode).
// ---------------------------------------------------------------------------

struct Idn2Bind {
    void* handle = nullptr;
    bool loaded = false;
    int (*lookup_u8)(const unsigned char*, unsigned char**, int) = nullptr;
    void (*free_fn)(void*) = nullptr;

    bool load() {
        if (loaded) return handle != nullptr;
        loaded = true;
        const char* names[] = {"libidn2.so.0", "libidn2.so", nullptr};
        for (int i = 0; names[i]; i++) {
            handle = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);
            if (handle) break;
        }
        if (!handle) return false;
        lookup_u8 = reinterpret_cast<decltype(lookup_u8)>(
            dlsym(handle, "idn2_lookup_u8"));
        free_fn = reinterpret_cast<decltype(free_fn)>(
            dlsym(handle, "idn2_free"));
        if (!lookup_u8 || !free_fn) {
            dlclose(handle);
            handle = nullptr;
            return false;
        }
        return true;
    }

    // Возвращает пустую строку при неудаче.
    std::string to_ascii(const std::string& host) {
        if (!load()) return "";
        unsigned char* out = nullptr;
        const int rc = lookup_u8(
            reinterpret_cast<const unsigned char*>(host.c_str()), &out, 0);
        if (rc != 0 || !out) return "";
        std::string res(reinterpret_cast<const char*>(out));
        free_fn(out);
        return res;
    }
};

Idn2Bind& idn2() {
    static Idn2Bind b;
    return b;
}

// Простейшее приведение к нижнему регистру (ASCII + кириллица). Для IDNA это
// достаточно в подавляющем большинстве случаев (полный Unicode case folding не
// нужен для кириллических доменов).
std::uint32_t fold_cp(std::uint32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 0x20;
    if (c >= 0x0410 && c <= 0x042F) return c + 0x20;  // русская А–Я
    if (c == 0x0401) return 0x0451;                   // Ё
    return c;
}

// Декодирование UTF-8 -> code points. Некорректные байты пропускаются.
std::vector<std::uint32_t> utf8_to_cp(const std::string& s) {
    std::vector<std::uint32_t> out;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        unsigned char b = p[i];
        std::uint32_t cp = 0;
        int len = 0;
        if (b < 0x80) { cp = b; len = 1; }
        else if ((b & 0xE0) == 0xC0) { cp = b & 0x1F; len = 2; }
        else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; len = 3; }
        else if ((b & 0xF8) == 0xF0) { cp = b & 0x07; len = 4; }
        else { i++; continue; }  // некорректный стартовый байт
        if (i + len > n) { i++; continue; }
        bool ok = true;
        for (int k = 1; k < len; k++) {
            if ((p[i + k] & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if (!ok) { i++; continue; }
        out.push_back(cp);
        i += len;
    }
    return out;
}

// Кодировщик punycode (RFC 3492) для одной метки.
std::string punycode_label(const std::string& label) {
    static const char kBase32[] =
        "abcdefghijklmnopqrstuvwxyz0123456789";
    const int kBase = 36, kTmin = 1, kTmax = 26, kSkew = 38, kDamp = 700;
    const int kInitialBias = 72, kInitialN = 128;

    std::vector<std::uint32_t> cp = utf8_to_cp(label);
    for (auto& c : cp) c = fold_cp(c);

    std::string out;
    int n = kInitialN;
    int delta = 0;
    int bias = kInitialBias;
    int h = 0;  // число basic code points
    int b_count = 0;
    for (std::uint32_t c : cp) {
        if (c < 0x80) { out += static_cast<char>(c); b_count++; h++; }
    }
    int basic = b_count;
    if (basic > 0 && basic < static_cast<int>(cp.size())) out += '-';

    auto adapt = [&](int d, int numpoints, bool first) {
        d = first ? d / kDamp : d / 2;
        d += d / numpoints;
        int k = 0;
        while (d > ((kBase - kTmin) * kTmax) / 2) {
            d /= (kBase - kTmin);
            k += kBase;
        }
        return k + (((kBase - kTmin + 1) * d) / (d + kSkew));
    };

    while (h < static_cast<int>(cp.size())) {
        int m = 0x7FFFFFFF;
        for (std::uint32_t c : cp) if (c >= n && c < (std::uint32_t)m) m = c;
        delta += (m - n) * (h + 1);
        n = m;
        for (std::uint32_t c : cp) {
            if (c < (std::uint32_t)n) delta++;
            if (c == (std::uint32_t)n) {
                int q = delta;
                for (int k = kBase;; k += kBase) {
                    int t = k <= bias ? kTmin : (k >= bias + kTmax ? kTmax : k - bias);
                    if (q < t) break;
                    out += kBase32[t + ((q - t) % (kBase - t))];
                    q = (q - t) / (kBase - t);
                }
                out += kBase32[q];
                bias = adapt(delta, h + 1, h == basic);
                delta = 0;
                h++;
            }
        }
        delta++;
        n++;
    }
    return out;
}

bool has_nonascii(const std::string& s) {
    for (unsigned char c : s) if (c >= 0x80) return true;
    return false;
}

// Преобразует хост (возможно, с точками) в ASCII. Целиком доверяем libidn2,
// иначе кодируем каждую метку отдельно.
std::string idna_to_ascii(const std::string& host) {
    if (!has_nonascii(host)) return host;
    std::string via = idn2().to_ascii(host);
    if (!via.empty()) return via;

    // Фолбэк: кодируем каждую метку, содержащую не-ASCII.
    std::string out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= host.size(); i++) {
        if (i == host.size() || host[i] == '.') {
            std::string label = host.substr(start, i - start);
            if (has_nonascii(label)) {
                out += "xn--" + punycode_label(label);
            } else {
                out += label;
            }
            if (i != host.size()) out += '.';
            start = i + 1;
        }
    }
    return out;
}

} // namespace

std::string normalize_url(const std::string& url) {
    // Без схемы (relative/protocol-relative) хост не выделить надёжно.
    const std::size_t scheme = url.find("://");
    if (scheme == std::string::npos) return url;

    const std::string scheme_part = url.substr(0, scheme + 3);  // "https://"
    const std::string rest = url.substr(scheme + 3);

    // Конец authority: первый из '/', '?', '#' (либо конец строки).
    std::size_t ae = rest.find('/');
    std::size_t aq = rest.find('?');
    std::size_t af = rest.find('#');
    std::size_t end = rest.size();
    if (ae != std::string::npos) end = ae;
    if (aq != std::string::npos && aq < end) end = aq;
    if (af != std::string::npos && af < end) end = af;

    const std::string authority = rest.substr(0, end);
    const std::string tail = rest.substr(end);  // путь/query/fragment

    // userinfo@host:port
    std::string userinfo;
    std::string hostport = authority;
    const std::size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        userinfo = authority.substr(0, at);
        hostport = authority.substr(at + 1);
    }

    // host и port
    std::string host;
    std::string port;
    if (!hostport.empty() && hostport[0] == '[') {
        // IPv6-литерал [....]
        const std::size_t rb = hostport.find(']');
        if (rb != std::string::npos) {
            host = hostport.substr(0, rb + 1);
            if (rb + 1 < hostport.size() && hostport[rb + 1] == ':') {
                port = hostport.substr(rb + 2);
            }
        } else {
            host = hostport;
        }
    } else {
        const std::size_t colon = hostport.rfind(':');
        if (colon != std::string::npos) {
            host = hostport.substr(0, colon);
            port = hostport.substr(colon + 1);
        } else {
            host = hostport;
        }
    }

    const std::string host_ascii = idna_to_ascii(host);

    std::string result = scheme_part;
    if (!userinfo.empty()) result += userinfo + "@";
    result += host_ascii;
    if (!port.empty()) result += ":" + port;
    result += tail;
    return result;
}

} // namespace news_rewriter
