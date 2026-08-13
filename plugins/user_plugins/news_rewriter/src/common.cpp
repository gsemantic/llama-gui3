#include "common.h"

#include <cstdint>
#include <cstring>
#include <ctime>

namespace news_rewriter {

const char* task_status_name(TaskStatus s) {
    switch (s) {
        case TaskStatus::Pending:    return "pending";
        case TaskStatus::Fetching:   return "fetching";
        case TaskStatus::Extracting: return "extracting";
        case TaskStatus::Rewriting:  return "rewriting";
        case TaskStatus::Exporting:  return "exporting";
        case TaskStatus::Done:       return "done";
        case TaskStatus::Error:      return "error";
    }
    return "?";
}

// ============================================================================
// SHA-256 (компактная реализация, без внешних зависимостей)
// ============================================================================

namespace {

const std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

inline std::uint32_t rotr(std::uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

} // namespace

std::string sha256_hex(const std::string& data) {
    std::uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    const std::size_t len = data.size();
    const std::size_t padded_len = ((len + 8) / 64 + 1) * 64;
    std::vector<unsigned char> msg(padded_len, 0);
    if (len) std::memcpy(msg.data(), data.data(), len);
    msg[len] = 0x80;
    const std::uint64_t bit_len = static_cast<std::uint64_t>(len) * 8;
    for (int i = 0; i < 8; i++) {
        msg[padded_len - 1 - i] = static_cast<unsigned char>(bit_len >> (i * 8));
    }

    for (std::size_t off = 0; off < padded_len; off += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = (static_cast<std::uint32_t>(msg[off + i * 4]) << 24) |
                   (static_cast<std::uint32_t>(msg[off + i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(msg[off + i * 4 + 2]) << 8) |
                   (static_cast<std::uint32_t>(msg[off + i * 4 + 3]));
        }
        for (int i = 16; i < 64; i++) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

        for (int i = 0; i < 64; i++) {
            const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
            const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }

        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (std::uint32_t v : h) {
        for (int i = 28; i >= 0; i -= 4) {
            out += hex[(v >> i) & 0x0F];
        }
    }
    return out;
}

std::string iso8601_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm_utc{};
    gmtime_r(&t, &tm_utc);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                  tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    return buf;
}

// ============================================================================
// Разбор времени ленты (RFC 822 / ISO 8601)
// ============================================================================

namespace {

// Дни с гражданской даты до эпохи (алгоритм Ховарда Хиннанта, proleptic
// Gregorian). Возвращает количество дней с 1970-01-01.
std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

int month_from_name(const std::string& s) {
    static const char* months[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                   "jul", "aug", "sep", "oct", "nov", "dec"};
    for (int i = 0; i < 12; i++) {
        if (s.size() >= 3 &&
            (s[0] | 0x20) == months[i][0] &&
            (s[1] | 0x20) == months[i][1] &&
            (s[2] | 0x20) == months[i][2]) {
            return i + 1;
        }
    }
    return 0;
}

int parse_int(const std::string& s) {
    if (s.empty()) return 0;
    std::int64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return 0;
        v = v * 10 + (c - '0');
        if (v > 100000) return 0;
    }
    return static_cast<int>(v);
}

// Смещение UTC в секундах из фрагмента "+HHMM", "+HH:MM", "Z" или "-HH:MM".
int parse_zone_offset(const std::string& z) {
    if (z.empty() || z == "Z" || z == "z" || z == "GMT" || z == "UTC") return 0;
    int sign = 1;
    std::size_t i = 0;
    if (z[0] == '+' || z[0] == '-') {
        if (z[0] == '-') sign = -1;
        i = 1;
    }
    int hh = 0, mm = 0;
    if (i < z.size() && z[i] >= '0' && z[i] <= '9') hh = parse_int(z.substr(i, 2));
    std::size_t j = i + 2;
    if (j < z.size() && z[j] == ':') j++;
    if (j < z.size() && z[j] >= '0' && z[j] <= '9') mm = parse_int(z.substr(j, 2));
    return sign * (hh * 3600 + mm * 60);
}

// RFC 822: [Wed, ]08 Aug 2026 11:51:47 [+0300|GMT]
bool parse_rfc822(const std::string& s, std::int64_t& out) {
    std::string t = s;
    const std::size_t comma = t.find(',');
    if (comma != std::string::npos) t = t.substr(comma + 1);
    std::string day_s, mon_s, year_s, time_s, zone_s;
    std::string rest = t;
    // Пропускаем ведущие пробелы.
    while (!rest.empty() && (rest[0] == ' ' || rest[0] == '\t')) rest.erase(0, 1);
    auto next_token = [&rest](std::string& tok) {
        tok.clear();
        while (!rest.empty() && (rest[0] == ' ' || rest[0] == '\t')) rest.erase(0, 1);
        while (!rest.empty() && rest[0] != ' ' && rest[0] != '\t') {
            tok += rest[0];
            rest.erase(0, 1);
        }
    };
    next_token(day_s);
    next_token(mon_s);
    next_token(year_s);
    next_token(time_s);
    next_token(zone_s);
    if (day_s.empty() || mon_s.empty() || year_s.empty() || time_s.empty()) return false;

    const int day = parse_int(day_s);
    const int month = month_from_name(mon_s);
    const int year = parse_int(year_s);
    if (day == 0 || month == 0 || year == 0) return false;

    int hh = 0, mm = 0, ss = 0;
    if (time_s.size() >= 2) hh = parse_int(time_s.substr(0, 2));
    if (time_s.size() >= 5) mm = parse_int(time_s.substr(3, 2));
    if (time_s.size() >= 8) ss = parse_int(time_s.substr(6, 2));
    const int zone = parse_zone_offset(zone_s);

    const std::int64_t days = days_from_civil(year, month, day);
    out = days * 86400 + hh * 3600 + mm * 60 + ss - zone;
    return true;
}

// ISO 8601: 2026-08-08T11:51:47[Z|+03:00|-0300]
bool parse_iso8601(const std::string& s, std::int64_t& out) {
    if (s.size() < 19) return false;
    if (!(s[0] >= '0' && s[0] <= '9')) return false;
    const int year = parse_int(s.substr(0, 4));
    const int month = parse_int(s.substr(5, 2));
    const int day = parse_int(s.substr(8, 2));
    const int hh = parse_int(s.substr(11, 2));
    const int mm = parse_int(s.substr(14, 2));
    const int ss = parse_int(s.substr(17, 2));
    if (year == 0 || month == 0 || day == 0) return false;
    const std::string zone_s = s.size() > 19 ? s.substr(19) : "";
    const int zone = parse_zone_offset(zone_s);
    const std::int64_t days = days_from_civil(year, month, day);
    out = days * 86400 + hh * 3600 + mm * 60 + ss - zone;
    return true;
}

} // namespace

std::int64_t parse_feed_time(const std::string& s) {
    std::int64_t out = 0;
    if (s.find('T') != std::string::npos || (s.size() >= 10 && s[4] == '-')) {
        if (parse_iso8601(s, out)) return out;
    }
    if (parse_rfc822(s, out)) return out;
    return 0;
}

std::string host_of(const std::string& url) {
    const std::size_t scheme = url.find("://");
    const std::size_t start = scheme == std::string::npos ? 0 : scheme + 3;
    const std::size_t end = url.find_first_of("/?#", start);
    std::string host = url.substr(start,
                                  end == std::string::npos ? std::string::npos : end - start);
    const std::size_t colon = host.rfind(':');
    if (colon != std::string::npos) host = host.substr(0, colon);
    return host.empty() ? url : host;
}

Json article_to_json(const Article& a) {
    Json j = Json::object();
    j["id"] = a.id;
    j["url"] = a.url;
    j["source"] = a.source;
    j["fetched_at"] = a.fetched_at;
    j["title_original"] = a.title_original;
    j["body_original"] = a.body_original;
    j["title_rewritten"] = a.title_rewritten;
    j["body_rewritten"] = a.body_rewritten;
    j["language"] = a.language;
    j["content_hash"] = a.content_hash;
    // Авто-SEO и заглавное изображение (заполняются конвейером, могут быть пусты).
    if (!a.source_image.empty()) j["source_image"] = a.source_image;
    if (!a.seo_focus_keyword.empty()) j["seo_focus_keyword"] = a.seo_focus_keyword;
    if (!a.seo_meta_description.empty()) j["seo_meta_description"] = a.seo_meta_description;
    if (!a.seo_title.empty()) j["seo_title"] = a.seo_title;
    return j;
}

} // namespace news_rewriter
