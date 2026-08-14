#include "extractor.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "common.h"  // parse_feed_time — для разбора <time datetime> и ISO-дат

namespace news_rewriter {

namespace {

// ---------------------------------------------------------------------------
// HTML → текст (лексический токенизатор, без внешних зависимостей)
// ---------------------------------------------------------------------------

// Блочные теги: после них текст начинается с новой строки.
bool is_block_tag(const std::string& name) {
    static const char* kBlocks[] = {
        "p", "div", "br", "li", "ul", "ol", "tr", "table", "h1", "h2",
        "h3", "h4", "h5", "h6", "blockquote", "pre", "section", "article",
        "header", "footer", "nav", "aside", "hr", "form", "figure", "figcaption",
    };
    for (const char* b : kBlocks) {
        if (name == b) return true;
    }
    return false;
}

// Теги, содержимое которых полностью выбрасывается (не текст новости).
bool is_skip_content_tag(const std::string& name) {
    static const char* kSkip[] = {
        "script", "style", "head", "noscript", "iframe", "template", "svg",
    };
    for (const char* s : kSkip) {
        if (name == s) return true;
    }
    return false;
}

// Имя тега в нижнем регистре, без атрибутов: "<div class=x>" → "div".
std::string tag_name(const std::string& html, std::size_t i, std::size_t* end) {
    std::size_t j = i + 1;
    if (j < html.size() && (html[j] == '/' || html[j] == '!')) ++j;
    std::string name;
    while (j < html.size()) {
        const unsigned char c = static_cast<unsigned char>(html[j]);
        if (std::isalnum(c) || c == '-' || c == '_') {
            name += static_cast<char>(std::tolower(c));
            ++j;
        } else {
            break;
        }
    }
    *end = j;
    return name;
}

// Поиск закрывающего тега "</name ...>"; возвращает индекс за '>'.
std::size_t find_close_tag(const std::string& html, std::size_t from,
                           const std::string& name) {
    const std::string close = "</" + name;
    std::size_t pos = from;
    while ((pos = html.find(close, pos)) != std::string::npos) {
        const std::size_t gt = html.find('>', pos + close.size());
        if (gt == std::string::npos) return std::string::npos;
        // после "</name" допустимы пробелы/атрибуты до '>'
        std::size_t k = pos + close.size();
        bool ok = true;
        while (k < gt) {
            if (!std::isspace(static_cast<unsigned char>(html[k]))) {
                ok = false;
                break;
            }
            ++k;
        }
        if (ok) return gt + 1;
        pos = gt + 1;
    }
    return std::string::npos;
}

// Декодирует сущность (без '&' и ';'); возвращает пустую строку, если незнакома.
std::string decode_entity(const std::string& ent) {
    if (ent == "amp")  return "&";
    if (ent == "lt")   return "<";
    if (ent == "gt")   return ">";
    if (ent == "quot") return "\"";
    if (ent == "apos") return "'";
    if (ent == "nbsp") return " ";
    if (ent.size() > 1 && ent[0] == '#') {
        const bool hex = ent.size() > 2 && (ent[1] == 'x' || ent[1] == 'X');
        const char* p = ent.c_str() + (hex ? 2 : 1);
        char* end = nullptr;
        const unsigned long code = std::strtoul(p, &end, hex ? 16 : 10);
        if (end && *end == '\0' && code > 0) {
            std::string out;
            if (code < 0x80) {
                out += static_cast<char>(code);
            } else if (code < 0x800) {
                out += static_cast<char>(0xC0 | (code >> 6));
                out += static_cast<char>(0x80 | (code & 0x3F));
            } else if (code < 0x10000) {
                out += static_cast<char>(0xE0 | (code >> 12));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (code >> 18));
                out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            }
            return out;
        }
    }
    return "";
}

// Число «букв» (ASCII-альфанумерика + все UTF-8 кодпоинты) в строке.
std::size_t letter_count(const std::string& s) {
    std::size_t count = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (std::isalnum(c)) {
            ++count;
            ++i;
        } else if (c >= 0xC0) {  // лидирующий байт UTF-8 → один «символ»
            ++count;
            ++i;
            while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
        } else {
            ++i;
        }
    }
    return count;
}

// Число «слов» (токенов, содержащих хотя бы одну букву).
std::size_t word_count(const std::string& s) {
    std::size_t count = 0;
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        if (std::isspace(static_cast<unsigned char>(s[i]))) {
            ++i;
            continue;
        }
        // токен от i до пробела
        const std::size_t start = i;
        while (i < n && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        const std::string tok = s.substr(start, i - start);
        if (letter_count(tok) > 0) ++count;
    }
    return count;
}

// Нормализация строк текста: трим, схлопывание пробелов, отсев пустых строк
// и коротких «навигационных» строк (меню, кнопки, подписи).
std::string normalize_lines(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    std::string line;
    auto flush = [&]() {
        // трим
        const std::size_t b = line.find_first_not_of(" \t\r");
        const std::size_t e = line.find_last_not_of(" \t\r");
        line = (b == std::string::npos) ? std::string() : line.substr(b, e - b + 1);
        // схлопывание внутренних пробелов
        std::string clean;
        clean.reserve(line.size());
        bool prev_space = false;
        for (char c : line) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                prev_space = true;
            } else {
                if (prev_space && !clean.empty()) clean += ' ';
                clean += c;
                prev_space = false;
            }
        }
        // отсев шума: слишком короткие однословные строки (меню/подписи)
        if (letter_count(clean) >= 6 && word_count(clean) >= 2) {
            if (!out.empty()) out += '\n';
            out += clean;
        }
        line.clear();
    };

    for (char c : in) {
        if (c == '\n') {
            flush();
        } else {
            line += c;
        }
    }
    flush();
    return out;
}

// Контейнеры, содержимое которых — не текст статьи (меню, шапка, подвал, сайдбар).
bool is_noise_container(const std::string& name) {
    static const char* kNoise[] = {"nav", "header", "footer", "aside", "form"};
    for (const char* s : kNoise) {
        if (name == s) return true;
    }
    return false;
}

// Заголовочные теги h1..h6: заголовок статьи извлекается отдельно.
bool is_heading_tag(const std::string& name) {
    return name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6';
}

// Число «символов» без пробелов (UTF-8-aware: многобайтовая последовательность
// считается одним символом — иначе для кириллицы density занижалась бы вдвое).
std::size_t nonspace_count(const std::string& s) {
    std::size_t c = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char ch = static_cast<unsigned char>(s[i]);
        if (std::isspace(ch)) {
            ++i;
            continue;
        }
        ++c;
        if (ch >= 0xC0) {
            ++i;
            while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
        } else {
            ++i;
        }
    }
    return c;
}

// Плотность текста: доля «букв» среди всех непустых символов (0..1).
// Низкая плотность — характерный признак мусора (счётчики, даты, символьные строки).
double text_density(const std::string& text) {
    const std::size_t ch = nonspace_count(text);
    if (ch == 0) return 0.0;
    return static_cast<double>(letter_count(text)) / static_cast<double>(ch);
}

// Значение атрибута внутри тега (attr="..." или attr='...').
std::string get_attr(const std::string& tag, const std::string& attr) {
    const std::string key = attr + "=";
    std::size_t p = 0;
    while ((p = tag.find(key, p)) != std::string::npos) {
        std::size_t v = p + key.size();
        while (v < tag.size() && (tag[v] == ' ' || tag[v] == '\t')) ++v;
        if (v < tag.size() && (tag[v] == '"' || tag[v] == '\'')) {
            const char quote = tag[v++];
            const std::size_t e = tag.find(quote, v);
            if (e != std::string::npos) return tag.substr(v, e - v);
        }
        p += key.size();
    }
    return "";
}

// URL из <meta property="..."> / <meta name="..."> с content="...".
std::string meta_image_url(const std::string& html, const std::string& prop) {
    std::size_t pos = 0;
    while ((pos = html.find("<meta", pos)) != std::string::npos) {
        const std::size_t gt = html.find('>', pos);
        if (gt == std::string::npos) break;
        const std::string tag = html.substr(pos, gt - pos + 1);
        const std::string p = get_attr(tag, "property");
        const std::string n = get_attr(tag, "name");
        if (p == prop || n == prop) {
            const std::string c = get_attr(tag, "content");
            if (!c.empty()) return c;
        }
        pos = gt + 1;
    }
    return "";
}

// Первый <img>: предпочитаем src, иначе data-src/data-lazy-src (lazy-load).
// Пропускаем data: URI.
std::string first_img_src(const std::string& html) {
    std::size_t pos = 0;
    while ((pos = html.find("<img", pos)) != std::string::npos) {
        const std::size_t gt = html.find('>', pos);
        if (gt == std::string::npos) break;
        const std::string tag = html.substr(pos, gt - pos + 1);
        std::string src = get_attr(tag, "src");
        if (src.empty()) src = get_attr(tag, "data-src");
        if (src.empty()) src = get_attr(tag, "data-lazy-src");
        if (!src.empty()) {
            if (src.find("data:") == 0) { pos = gt + 1; continue; }
            return src;
        }
        pos = gt + 1;
    }
    return "";
}

// Заглавное изображение: og:image → twitter:image → первый <img>.
std::string extract_image_url(const std::string& html) {
    std::string u = meta_image_url(html, "og:image");
    if (u.empty()) u = meta_image_url(html, "twitter:image");
    if (u.empty()) u = first_img_src(html);
    return u;
}

// --- helpers для разбора страниц-списков (page mode) ----------------------

// Резолв относительного href в абсолютный URL по базовому адресу страницы.
std::string resolve_page_url(const std::string& href, const std::string& base) {
    if (href.empty() || base.empty()) return href;
    if (href.find("://") != std::string::npos) return href;        // уже абсолютный
    if (href.size() >= 2 && href[0] == '/' && href[1] == '/') {    // //host/path
        const std::size_t scheme = base.find("://");
        if (scheme == std::string::npos) return href;
        return base.substr(0, scheme + 3) + href.substr(2);
    }
    const std::size_t scheme = base.find("://");
    if (scheme == std::string::npos) return href;
    const std::size_t host_end = base.find('/', scheme + 3);
    const std::string origin = host_end == std::string::npos
                                   ? base : base.substr(0, host_end);
    if (!href.empty() && href[0] == '/') return origin + href;     // /path
    const std::size_t last_slash = base.rfind('/');
    const std::string dir = last_slash != std::string::npos
                                ? base.substr(0, last_slash + 1) : origin + "/";
    return dir + href;
}

// Первый <img> в диапазоне [from, to) HTML: предпочитаем src, иначе
// data-src/data-lazy-src (lazy-load). Пропускаем data: URI.
std::string first_img_in_range(const std::string& html, std::size_t from,
                               std::size_t to) {
    std::size_t pos = from;
    while ((pos = html.find("<img", pos)) != std::string::npos && pos < to) {
        std::size_t gt = html.find('>', pos);
        if (gt == std::string::npos || gt >= to) break;
        const std::string tag = html.substr(pos, gt - pos + 1);
        std::string src = get_attr(tag, "src");
        if (src.empty()) src = get_attr(tag, "data-src");
        if (src.empty()) src = get_attr(tag, "data-lazy-src");
        if (!src.empty()) {
            if (src.find("data:") == 0) { pos = gt + 1; continue; }
            return src;
        }
        pos = gt + 1;
    }
    return "";
}

// Текст первого заголовка h1..h3 внутри блока HTML (без вложенных тегов).
std::string first_heading_text(const std::string& html) {
    static const char* kH[] = {"h1", "h2", "h3"};
    for (const char* h : kH) {
        const std::string open = std::string("<") + h;
        std::size_t p = html.find(open);
        while (p != std::string::npos) {
            const std::size_t gt = html.find('>', p);
            if (gt == std::string::npos) break;
            const std::size_t close = html.find("</" + std::string(h), gt);
            if (close == std::string::npos) break;
            const std::string t =
                html_to_text(html.substr(gt + 1, close - (gt + 1)));
            if (!t.empty()) return t;
            p = html.find(open, close);
        }
    }
    return "";
}

// Текст первой ссылки <a>...</a> в блоке HTML.
std::string first_anchor_text(const std::string& html) {
    std::size_t p = html.find("<a ");
    if (p == std::string::npos) p = html.find("<a>");
    if (p == std::string::npos) return "";
    const std::size_t gt = html.find('>', p);
    if (gt == std::string::npos) return "";
    const std::size_t close = html.find("</a", gt);
    if (close == std::string::npos) return "";
    return html_to_text(html.substr(gt + 1, close - (gt + 1)));
}

// href первой ссылки <a> в блоке HTML.
std::string first_anchor_href(const std::string& html) {
    std::size_t p = html.find("<a ");
    if (p == std::string::npos) return "";
    const std::size_t gt = html.find('>', p);
    if (gt == std::string::npos) return "";
    return get_attr(html.substr(p, gt - p + 1), "href");
}

// --- разбор даты публикации (для фильтра «свежесть» в page-режиме) ----------

// Месяц по названию (рус + англ). Возвращает 1..12 или 0.
int month_from_name_ruen(const std::string& raw) {
    std::string s;
    s.reserve(raw.size());
    for (char c : raw) {
        if (c >= 'A' && c <= 'Z') s += static_cast<char>(c + ('a' - 'A'));
        else s += c;
    }
    static const char* en[12] = {"jan", "feb", "mar", "apr", "may", "jun",
                                 "jul", "aug", "sep", "oct", "nov", "dec"};
    for (int i = 0; i < 12; ++i) {
        if (s.size() >= 3 && s[0] == en[i][0] && s[1] == en[i][1] &&
            s[2] == en[i][2]) {
            return i + 1;
        }
    }
    static const char* ru[12][2] = {
        {"января", "янв"}, {"февраля", "фев"}, {"марта", "мар"}, {"апреля", "апр"},
        {"мая", "май"}, {"июня", "июн"}, {"июля", "июл"}, {"августа", "авг"},
        {"сентября", "сен"}, {"октября", "окт"}, {"ноября", "ноя"}, {"декабря", "дек"}
    };
    for (int i = 0; i < 12; ++i) {
        if (s == ru[i][0] || s == ru[i][1]) return i + 1;
    }
    return 0;
}

bool is_year(const std::string& s) {
    if (s.size() != 4) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    const int y = std::atoi(s.c_str());
    return y >= 1900 && y <= 2999;
}

bool is_digits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

int digits_to_int(const std::string& s) {
    int v = 0;
    for (char c : s) {
        v = v * 10 + (c - '0');
        if (v > 100000) break;
    }
    return v;
}

std::string two_digit(int n) {
    std::string s;
    if (n < 10) s += '0';
    s += std::to_string(n);
    return s;
}

// Токенизация текста: буквы/цифры и внутренние . - : / , остаются в токене,
// остальная пунктуация — разделитель. Краевая пунктуация (.,;,) отсекается.
std::vector<std::string> date_tokens(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty()) { out.push_back(cur); cur.clear(); }
    };
    for (char c : text) {
        const unsigned char uc = static_cast<unsigned char>(c);
        // uc >= 0x80 — любой не-ASCII байт UTF-8 (лидирующий и продолжение),
        // чтобы названия месяцев на русском не терялись при токенизации.
        const bool keep = (uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z') ||
                          (uc >= '0' && uc <= '9') || uc >= 0x80 ||
                          uc == '.' || uc == '-' || uc == ':' || uc == '/' ||
                          uc == ',';
        if (keep) cur += c;
        else flush();
    }
    flush();
    for (auto& tk : out) {
        while (!tk.empty() && (tk.back() == ',' || tk.back() == '.' || tk.back() == ';'))
            tk.pop_back();
        while (!tk.empty() && (tk.front() == ',' || tk.front() == '.' || tk.front() == ';'))
            tk.erase(0, 1);
    }
    return out;
}

// Разбор текстовой даты (без <time datetime>). Поддерживает:
//  - ISO: YYYY-MM-DD[...] (передаётся в parse_feed_time)
//  - точечную: DD.MM.YYYY / DD.MM.YY
//  - «число месяц год» и «месяц число, год» (рус/англ названия месяцев)
// Возвращает секунды UTC (0 если не распознано).
std::int64_t parse_text_date(const std::string& text) {
    std::vector<std::string> toks = date_tokens(text);
    for (std::size_t i = 0; i + 2 < toks.size(); ++i) {
        const std::string& a = toks[i];
        const std::string& b = toks[i + 1];
        const std::string& c = toks[i + 2];
        int y = -1, m = -1, d = -1;

        // ISO YYYY-MM-DD (возможно с временем в том же токене).
        if (a.size() >= 10 && a[4] == '-' && is_digits(a.substr(0, 4))) {
            std::string iso = a;
            if (iso.find('T') == std::string::npos) iso += "T00:00:00";
            const std::int64_t t = parse_feed_time(iso);
            if (t > 0) return t;
        }

        // Точечная дата DD.MM.YYYY / DD.MM.YY.
        if (y < 0 && a.find('.') != std::string::npos) {
            std::vector<std::string> parts;
            std::string cur;
            for (char ch : a) {
                if (ch == '.') { parts.push_back(cur); cur.clear(); }
                else cur += ch;
            }
            parts.push_back(cur);
            if (parts.size() == 3 && is_digits(parts[0]) && is_digits(parts[1]) &&
                is_digits(parts[2])) {
                d = digits_to_int(parts[0]);
                m = digits_to_int(parts[1]);
                const int yy = digits_to_int(parts[2]);
                y = (parts[2].size() == 2) ? 2000 + yy : yy;
            }
        }

        // «число месяц год» (13 августа 2026 / 13 aug 2026).
        if (y < 0 && is_digits(a) && month_from_name_ruen(b) && is_year(c)) {
            d = digits_to_int(a);
            m = month_from_name_ruen(b);
            y = digits_to_int(c);
        }
        // «месяц число, год» (Aug 13, 2026).
        if (y < 0 && month_from_name_ruen(a) && is_digits(b) && is_year(c)) {
            m = month_from_name_ruen(a);
            d = digits_to_int(b);
            y = digits_to_int(c);
        }

        int hh = 0, mm = 0;
        if (y > 0 && m > 0 && d > 0 && i + 3 < toks.size()) {
            const std::string& e = toks[i + 3];
            const std::size_t colon = e.find(':');
            if (colon != std::string::npos && is_digits(e.substr(0, colon))) {
                hh = digits_to_int(e.substr(0, colon));
                const std::size_t c2 = e.find(':', colon + 1);
                if (c2 != std::string::npos) {
                    mm = digits_to_int(e.substr(colon + 1, c2 - colon - 1));
                } else {
                    mm = digits_to_int(e.substr(colon + 1));
                }
            }
        }

        if (y > 0 && m > 0 && d > 0) {
            const std::string iso = std::to_string(y) + "-" + two_digit(m) + "-" +
                                    two_digit(d) + "T" + two_digit(hh) + ":" +
                                    two_digit(mm) + ":00";
            return parse_feed_time(iso);
        }
    }
    return 0;
}

// Дата публикации из блока HTML: сначала <time datetime="...">, затем попытка
// распознать текстовую дату в тексте блока. Возвращает 0, если неизвестно.
std::int64_t extract_published_at(const std::string& block) {
    std::size_t p = block.find("<time");
    while (p != std::string::npos) {
        const std::size_t gt = block.find('>', p);
        if (gt == std::string::npos) break;
        const std::string tag = block.substr(p, gt - p + 1);
        const std::string dt = get_attr(tag, "datetime");
        if (!dt.empty()) {
            std::string norm = dt;
            if (norm.find('T') == std::string::npos && norm.size() >= 10 &&
                norm[4] == '-') {
                norm += "T00:00:00";  // только дата → полночь UTC
            }
            const std::int64_t t = parse_feed_time(norm);
            if (t > 0) return t;
        }
        p = gt + 1;
    }
    return parse_text_date(html_to_text(block));
}

// Поля одной статьи из блока HTML (напр. внутри <article>): заголовок,
// ссылка, изображение, дата публикации и текст-сниппет.
ExtractedArticle item_fields_from_block(const std::string& block,
                                        const std::string& base) {
    ExtractedArticle it;
    it.body = html_to_text(block);
    it.title = first_heading_text(block);
    if (it.title.empty()) it.title = first_anchor_text(block);
    it.url = first_anchor_href(block);
    if (!it.url.empty()) it.url = resolve_page_url(it.url, base);
    it.image = first_img_src(block);
    if (!it.image.empty() && it.image.find("data:") != 0)
        it.image = resolve_url(it.image, base);
    it.published_at = extract_published_at(block);
    return it;
}

// Похоже ли href на ссылку на отдельную статью (отсекаем навигацию/ассеты).
bool is_article_href(const std::string& href) {
    if (href.empty()) return false;
    if (href.compare(0, 11, "javascript:") == 0) return false;
    if (href.compare(0, 7, "mailto:") == 0) return false;
    if (href.compare(0, 4, "tel:") == 0) return false;
    if (href[0] == '#') return false;
    const std::size_t q = href.find('?');
    const std::string path = (q == std::string::npos) ? href : href.substr(0, q);
    const std::size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) {
        const std::string ext = path.substr(dot + 1);
        if (ext == "css" || ext == "js" || ext == "json" || ext == "xml" ||
            ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" ||
            ext == "webp" || ext == "svg" || ext == "ico" || ext == "pdf") {
            return false;
        }
    }
    return true;
}

// forward declarations (определены ниже в этом namespace)
bool has_noise_class(const std::string& tag);
std::size_t find_tag_open(const std::string& html, std::size_t pos,
                          const std::string& tag);

// Поиск блоков статей по тегу <article> (основной путь для современных сайтов).
std::vector<ExtractedArticle> detect_article_tags(const std::string& html,
                                                  const std::string& base) {
    std::vector<ExtractedArticle> items;
    std::size_t p = 0;
    while ((p = find_tag_open(html, p, "article")) != std::string::npos) {
        const std::size_t gt = html.find('>', p);
        if (gt == std::string::npos) break;
        const std::string open_tag = html.substr(p, gt - p + 1);
        // «Постоянная правая информация» часто лежит в <article class="...sidebar/widget/...">.
        if (has_noise_class(open_tag)) { p = gt + 1; continue; }
        const std::size_t close = find_close_tag(html, gt + 1, "article");
        const std::size_t end = (close == std::string::npos) ? html.size() : close;
        const std::string block = html.substr(gt + 1, end - (gt + 1));
        ExtractedArticle it = item_fields_from_block(block, base);
        if (!it.body.empty() || !it.title.empty()) items.push_back(std::move(it));
        p = (close == std::string::npos) ? html.size() : close;
    }
    return items;
}

// Запасной путь: повторяющиеся ссылки на новости (заголовок + ссылка). Для
// каждой — сниппет = текст ссылки, изображение ищем в сегменте до следующей
// ссылки.
std::vector<ExtractedArticle> detect_anchor_items(const std::string& html,
                                                 const std::string& base) {
    struct Anchor { std::string href; std::string text; std::size_t pos; };
    std::vector<Anchor> anchors;
    std::size_t p = 0;
    while ((p = html.find("<a ", p)) != std::string::npos) {
        const std::size_t gt = html.find('>', p);
        if (gt == std::string::npos) break;
        const std::string tag = html.substr(p, gt - p + 1);
        const std::string href = get_attr(tag, "href");
        const std::size_t close = html.find("</a", gt);
        const std::size_t end = (close == std::string::npos) ? html.size() : close;
        const std::string text =
            html_to_text(html.substr(gt + 1, end - (gt + 1)));
        if (is_article_href(href) && letter_count(text) >= 15) {
            Anchor a;
            a.href = resolve_page_url(href, base);
            a.text = text;
            a.pos = p;
            anchors.push_back(std::move(a));
        }
        p = gt + 1;
    }
    std::vector<ExtractedArticle> items;
    for (std::size_t i = 0; i < anchors.size(); ++i) {
        const std::size_t from = anchors[i].pos;
        const std::size_t to = (i + 1 < anchors.size()) ? anchors[i + 1].pos
                                                        : html.size();
        ExtractedArticle it;
        it.title = anchors[i].text;
        it.url = anchors[i].href;
        it.body = anchors[i].text;
        it.image = first_img_in_range(html, from, to);
        if (!it.image.empty() && it.image.find("data:") != 0)
            it.image = resolve_url(it.image, base);
        it.published_at = extract_published_at(html.substr(from, to - from));
        items.push_back(std::move(it));
    }
    return items;
}

// Подстроки в class/id/role, по которым контейнер считается «не статьёй»
// (сайдбар, виджет, реклама, промо, «постоянная правая информация» и т.п.).
bool has_noise_class(const std::string& tag) {
    static const char* kBad[] = {
        "sidebar", "side-bar", "side_bar", "widget", "banner", "promo",
        "advert", "ad-", "-ad", "sponsor", "rightcol", "right-col",
        "rightcolumn", "rail", "popular", "most-read",
        "recommended", "related", "teaser",
        // Баннеры согласия на cookies / приватность / подписка / модалки:
        // часто единственный «связный» кусок текста на странице, который иначе
        // выигрывает у реальной новости в эвристике плотности.
        "cookie", "consent", "cky", "cmplz", "gdpr", "privacy",
        "policy", "terms", "subscribe", "newsletter", "modal", "overlay",
        "dialog", "popup"
    };
    // Контейнеры основного контента никогда не считаем шумом, даже если в
    // составном class случайно встречается слово из kBad (пример:
    // «content__main_with-aside» содержит «aside», но это главная колонка с
    // новостями, а не сайдбар). Без этой защиты полностью вырезается лента.
    static const char* kKeep[] = {
        "content", "article", "post", "news", "entry", "story"
    };
    const std::string cls = get_attr(tag, "class") + " " + get_attr(tag, "id") +
                            " " + get_attr(tag, "role");
    if (cls.empty()) return false;
    for (const char* k : kKeep) {
        if (cls.find(k) != std::string::npos) return false;
    }
    for (const char* b : kBad) {
        if (cls.find(b) != std::string::npos) return true;
    }
    return false;
}

// Находит открывающий тег <tag с учётом границы имени (за <tag идёт пробел,
// '>', '/' или перевод строки), начиная с pos. Возвращает позицию '<' или npos.
std::size_t find_tag_open(const std::string& html, std::size_t pos,
                          const std::string& tag) {
    const std::string pat = "<" + tag;
    std::size_t p = pos;
    while ((p = html.find(pat, p)) != std::string::npos) {
        const char c = (p + pat.size() < html.size()) ? html[p + pat.size()] : '>';
        if (c == ' ' || c == '>' || c == '/' || c == '\t' || c == '\n') return p;
        p += pat.size();
    }
    return std::string::npos;
}

// Удаляет поддерево тега tag, начиная с открывающего '<' на open_pos, с учётом
// вложенности одноимённых тегов. Изменяет html, возвращает новую позицию.
std::size_t erase_tag_subtree(std::string& html, std::size_t open_pos,
                              const std::string& tag) {
    const std::size_t gt = html.find('>', open_pos);
    if (gt == std::string::npos) return open_pos + 1;
    const std::string close = "</" + tag;
    int depth = 1;
    std::size_t p = gt + 1;
    while (p < html.size()) {
        const std::size_t no = find_tag_open(html, p, tag);
        const std::size_t nc = html.find(close, p);
        if (nc == std::string::npos) break;
        if (no != std::string::npos && no < nc) {
            ++depth;
            p = no + 1;
        } else {
            --depth;
            p = nc + close.size();
            if (depth == 0) break;
        }
    }
    html.erase(open_pos, p - open_pos);
    return open_pos;
}

// Вырезает из HTML «не-статьи»: сайдбары/виджеты/рекламу/навигацию. Удаляет
// целиком контейнеры aside/nav/footer/header/form и div/section с «шумным»
// class/id. Возвращает очищенный HTML, из которого дальше ищутся статьи.
std::string strip_non_article_regions(const std::string& html) {
    std::string out = html;
    for (const char* t : {"aside", "nav", "footer", "header", "form"}) {
        std::size_t p = 0;
        while ((p = find_tag_open(out, p, t)) != std::string::npos) {
            p = erase_tag_subtree(out, p, t);
        }
    }
    for (const char* t : {"div", "section"}) {
        std::size_t p = 0;
        while ((p = find_tag_open(out, p, t)) != std::string::npos) {
            const std::size_t gt = out.find('>', p);
            if (gt == std::string::npos) break;
            const std::string tag = out.substr(p, gt - p + 1);
            if (has_noise_class(tag)) {
                p = erase_tag_subtree(out, p, t);
            } else {
                p = gt + 1;
            }
        }
    }
    return out;
}

} // namespace

std::string html_to_text(const std::string& html) {
    std::string out;
    out.reserve(html.size());
    const std::size_t n = html.size();
    std::size_t i = 0;

    while (i < n) {
        const char c = html[i];

        if (c == '<') {
            // комментарий
            if (html.compare(i, 4, "<!--") == 0) {
                const std::size_t end = html.find("-->", i + 4);
                i = (end == std::string::npos) ? n : end + 3;
                continue;
            }
            // закрывающий тег
            if (i + 1 < n && html[i + 1] == '/') {
                const std::size_t gt = html.find('>', i);
                i = (gt == std::string::npos) ? n : gt + 1;
                continue;
            }
            std::size_t name_end = 0;
            const std::string name = tag_name(html, i, &name_end);
            const std::size_t gt = html.find('>', i);
            if (gt == std::string::npos) break;

            if (is_skip_content_tag(name)) {
                // выбросить всё содержимое до закрывающего тега
                const std::size_t close = find_close_tag(html, gt + 1, name);
                i = (close == std::string::npos) ? n : close;
                continue;
            }
            if (is_block_tag(name)) {
                if (!out.empty() && out.back() != '\n') out += '\n';
            }
            i = gt + 1;
            (void)name_end;
            continue;
        }

        if (c == '&') {
            const std::size_t semi = html.find(';', i);
            if (semi != std::string::npos && semi - i <= 12) {
                const std::string dec = decode_entity(html.substr(i + 1, semi - i - 1));
                if (!dec.empty()) {
                    out += dec;
                    i = semi + 1;
                    continue;
                }
            }
            out += c;
            ++i;
            continue;
        }

        out += c;
        ++i;
    }

    return normalize_lines(out);
}

ExtractedArticle extract_from_description(const std::string& desc) {
    ExtractedArticle result;
    result.body = html_to_text(desc);
    result.image = extract_image_url(desc);
    return result;
}

std::string first_content_image(const std::string& html) {
    // Декоративные/служебные картинки, которые не являются фото статьи.
    static const char* kSkip[] = {
        "logo", "icon", "og-images", "og_image", "preview", "social",
        "teaser", "watermark", "wm_", "spacer", "sprite", "placeholder",
        "banner", "advert", "pixel", "blank", "1x1", "tracking", "avatar"
    };
    std::size_t pos = 0;
    while ((pos = html.find("<img", pos)) != std::string::npos) {
        const std::size_t gt = html.find('>', pos);
        if (gt == std::string::npos) break;
        const std::string tag = html.substr(pos, gt - pos + 1);
        std::string src = get_attr(tag, "src");
        if (src.empty()) src = get_attr(tag, "data-src");
        if (src.empty()) src = get_attr(tag, "data-lazy-src");
        if (!src.empty() && src.find("data:") != 0) {
            bool skip = false;
            const std::string l = src;
            for (const char* k : kSkip) {
                if (l.find(k) != std::string::npos) { skip = true; break; }
            }
            if (!skip) return src;
        }
        pos = gt + 1;
    }
    return "";
}



namespace {

// Заголовок страницы: <h1> (заголовок статьи) или <title> (fallback).
std::string extract_title(const std::string& html) {
    // <h1>
    const std::size_t h_open = html.find("<h1");
    if (h_open != std::string::npos) {
        const std::size_t h_gt = html.find('>', h_open);
        const std::size_t h_close = html.find("</h1", h_gt);
        if (h_gt != std::string::npos && h_close != std::string::npos) {
            const std::string h = html_to_text(html.substr(h_gt + 1, h_close - h_gt - 1));
            if (!h.empty()) return h;
        }
    }
    // <title>
    const std::size_t t_open = html.find("<title");
    if (t_open != std::string::npos) {
        const std::size_t t_gt = html.find('>', t_open);
        const std::size_t t_close = html.find("</title", t_gt);
        if (t_gt != std::string::npos && t_close != std::string::npos) {
            const std::string t = html_to_text(html.substr(t_gt + 1, t_close - t_gt - 1));
            if (!t.empty()) return t;
        }
    }
    return "";
}

// Тело страницы: эвристика по плотности текста. HTML разбивается на блоки по
// блочным тегам; блоки внутри nav/header/footer/aside/form и заголовки h1..h6
// исключаются как заведомо не-статья. Среди оставшихся блоков «прозой»
// считаются те, что содержат достаточно слов/букв и высокую плотность текста;
// берётся самый длинный связный набор таких блоков — это и есть текст статьи.
std::string extract_body(const std::string& html) {
    enum class BlockKind { kEmpty, kCandidate, kOther };

    struct Block {
        std::string html;
        bool noise;    // внутри nav/header/footer/aside/form
        bool heading;  // внутри h1..h6
    };

    // -- разбор HTML на блоки по блочным тегам --------------------------------
    std::vector<Block> blocks;
    const std::size_t n = html.size();
    std::size_t i = 0;
    Block cur;
    std::size_t noise_depth = 0;        // nav/header/footer/aside/form
    std::size_t noise_class_depth = 0;  // div/section с «шумным» классом
    // стек: для каждого открытого div/section — был ли у него «шумный» класс.
    std::vector<bool> noise_class_stack;
    bool heading_flag = false;

    auto flush = [&]() {
        blocks.push_back(cur);
        cur = Block{};
    };

    while (i < n) {
        if (html[i] == '<') {
            if (html.compare(i, 4, "<!--") == 0) {
                const std::size_t end = html.find("-->", i + 4);
                i = (end == std::string::npos) ? n : end + 3;
                continue;
            }
            if (i + 1 < n && html[i + 1] == '/') {
                std::size_t name_end = 0;
                const std::string name = tag_name(html, i, &name_end);
                if (is_noise_container(name) && noise_depth > 0) --noise_depth;
                if ((name == "div" || name == "section") && !noise_class_stack.empty()) {
                    if (noise_class_stack.back()) --noise_class_depth;
                    noise_class_stack.pop_back();
                }
                const std::size_t gt = html.find('>', i);
                if (gt == std::string::npos) break;
                cur.html += " ";
                i = gt + 1;
                continue;
            }
            std::size_t name_end = 0;
            const std::string name = tag_name(html, i, &name_end);
            const std::size_t gt = html.find('>', i);
            if (gt == std::string::npos) break;

            if (is_skip_content_tag(name)) {
                const std::size_t close = find_close_tag(html, gt + 1, name);
                i = (close == std::string::npos) ? n : close;
                continue;
            }
            if (is_block_tag(name)) {
                flush();
                if (is_noise_container(name)) ++noise_depth;
                if (name == "div" || name == "section") {
                    const bool is_nc = has_noise_class(html.substr(i, gt - i + 1));
                    noise_class_stack.push_back(is_nc);
                    if (is_nc) ++noise_class_depth;
                }
                if (is_heading_tag(name)) heading_flag = true;
                // флаги фиксируются при создании блока, чтобы закрывающий
                // тег (например, </nav>) не «снял» их с уже идущего текста.
                cur.noise = (noise_depth > 0 || noise_class_depth > 0);
                cur.heading = heading_flag;
                heading_flag = false;
            }
            i = gt + 1;
            (void)name_end;
            continue;
        }
        cur.html += html[i];
        ++i;
    }
    flush();

    // -- оценка каждого блока --------------------------------------------------
    struct Scored {
        std::string text;
        std::size_t letters = 0;
        std::size_t words = 0;
        double density = 0.0;
        BlockKind kind = BlockKind::kOther;
    };
    std::vector<Scored> scored(blocks.size());
    for (std::size_t b = 0; b < blocks.size(); ++b) {
        const Block& blk = blocks[b];
        Scored& s = scored[b];
        s.text = html_to_text(blk.html);
        s.letters = letter_count(s.text);
        s.words = word_count(s.text);
        s.density = text_density(s.text);
        if (blk.noise || blk.heading) {
            s.kind = BlockKind::kOther;      // заведомо не-статья
        } else if (s.text.empty()) {
            s.kind = BlockKind::kEmpty;      // пустышка не рвёт связку абзацев
        } else if (s.words >= 2 && s.letters >= 15 && s.density >= 0.5) {
            s.kind = BlockKind::kCandidate;  // «проза» — кандидат в статью
        } else {
            s.kind = BlockKind::kOther;      // навигация/мусор — рвёт связку
        }
    }

    // -- выбор самого длинного связного набора «прозы» -------------------------
    std::size_t best_start = 0, best_end = 0, best_len = 0, best_letters = 0;
    std::size_t k = 0;
    while (k < scored.size()) {
        if (scored[k].kind == BlockKind::kOther) {
            ++k;
            continue;
        }
        const std::size_t start = k;
        std::size_t letters_sum = 0, len = 0;
        while (k < scored.size() && scored[k].kind != BlockKind::kOther) {
            if (scored[k].kind == BlockKind::kCandidate) {
                letters_sum += scored[k].letters;
                ++len;
            }
            ++k;
        }
        if (len > best_len || (len == best_len && letters_sum > best_letters)) {
            best_start = start;
            best_end = k;
            best_len = len;
            best_letters = letters_sum;
        }
    }

    // -- сборка результата ------------------------------------------------------
    std::string out;
    if (best_len > 0) {
        for (std::size_t b = best_start; b < best_end; ++b) {
            if (scored[b].kind != BlockKind::kCandidate) continue;
            if (!out.empty()) out += '\n';
            out += scored[b].text;
        }
        return out;
    }

    // нет ни одного подходящего блока — fallback: самый длинный блок текста.
    std::size_t best_i = 0, best_l = 0;
    for (std::size_t b = 0; b < scored.size(); ++b) {
        if (scored[b].letters > best_l) {
            best_l = scored[b].letters;
            best_i = b;
        }
    }
    return best_l > 0 ? scored[best_i].text : std::string();
}

} // namespace

ExtractedArticle extract_page(const std::string& html, const std::string& base_url,
                            const SourceExtract& cfg) {
    ExtractedArticle result;

    // План A: заданы маркеры — берём текст между ними.
    if (!cfg.body_marker.empty() || !cfg.title_marker.empty()) {
        if (!cfg.body_marker.empty()) {
            const std::size_t start = html.find(cfg.body_marker);
            if (start != std::string::npos) {
                const std::string rest = html.substr(start + cfg.body_marker.size());
                result.body = html_to_text(rest);
            }
        }
        if (!cfg.title_marker.empty()) {
            const std::size_t start = html.find(cfg.title_marker);
            if (start != std::string::npos) {
                const std::string rest = html.substr(start + cfg.title_marker.size());
                // до первого закрывающего тега или перевода строки
                std::size_t end = rest.find("</");
                const std::size_t nl = rest.find('\n');
                if (nl != std::string::npos && (end == std::string::npos || nl < end)) {
                    end = nl;
                }
                result.title = html_to_text(
                    rest.substr(0, end == std::string::npos ? std::string::npos : end));
            }
        }
        return result;
    }

    // План B: эвристика.
    result.title = extract_title(html);
    result.body = extract_body(html);
    // Картинка: приоритет — первое содержательное фото из тела статьи
    // (без логотипа сайта и наложения заголовка, которые обычно несёт
    // og:image). og:image / twitter:image берём только как запасной вариант.
    const std::string cleaned = strip_non_article_regions(html);
    std::string img = first_content_image(cleaned);
    if (img.empty()) img = extract_image_url(html);
    if (!img.empty() && img.find("data:") != 0) img = resolve_url(img, base_url);
    result.image = img;
    return result;
}

std::vector<ExtractedArticle> extract_page_items(const std::string& html,
                                                const std::string& base_url,
                                                const SourceExtract& cfg) {
    // Маркеры заданы — это одна конкретная статья, возвращаем как есть.
    if (!cfg.body_marker.empty() || !cfg.title_marker.empty()) {
        std::vector<ExtractedArticle> v;
        v.push_back(extract_page(html, base_url, cfg));
        return v;
    }

    // Убираем «постоянную» информацию (сайдбары/виджеты/рекламу), чтобы она не
    // попала в выдачу как отдельная статья, затем ищем блоки новостей.
    const std::string cleaned = strip_non_article_regions(html);

    // Список/категория: сначала по тегам <article> (основной путь).
    std::vector<ExtractedArticle> items = detect_article_tags(cleaned, base_url);
    if (items.size() >= 2) return items;

    // Запасной путь: повторяющиеся ссылки на новости.
    std::vector<ExtractedArticle> anchors = detect_anchor_items(cleaned, base_url);
    if (anchors.size() >= 2) return anchors;

    // Не список — вся страница как одна статья (поведение extract_page).
    std::vector<ExtractedArticle> single;
    single.push_back(extract_page(cleaned, base_url, cfg));
    return single;
}

std::vector<ExtractionProposal> extract_page_candidates(
        const std::string& html,
        const std::string& base_url,
        const SourceExtract& cfg) {
    std::vector<ExtractionProposal> out;

    // Маркеры заданы — детерминированное извлечение, ровно один вариант.
    if (!cfg.body_marker.empty() || !cfg.title_marker.empty()) {
        ExtractionProposal p;
        p.article = extract_page(html, base_url, cfg);
        p.strategy = 0;
        p.strategy_name = "маркеры";
        p.score = static_cast<double>(letter_count(p.article.body));
        out.push_back(std::move(p));
        return out;
    }

    const std::string cleaned = strip_non_article_regions(html);

    // Стратегия 0: полный авто (заголовок из h1/<title>, тело по density,
    // обложка — первое содержательное фото из тела статьи).
    {
        ExtractionProposal p;
        p.article.title = extract_title(html);
        p.article.body = extract_body(html);
        p.article.image = first_content_image(cleaned);
        if (p.article.image.empty()) p.article.image = extract_image_url(html);
        p.strategy = 0;
        p.strategy_name = "авто (density + фото из тела)";
        p.score = static_cast<double>(letter_count(p.article.body));
        out.push_back(std::move(p));
    }
    // Стратегия 1: тот же текст, но обложка — og:image / twitter:image (герой-
    // картинка сайта, часто несущая наложение заголовка/логотипа, которое
    // first_content_image намеренно пропускает).
    {
        ExtractionProposal p = out.front();
        p.article.image = extract_image_url(html);
        p.strategy = 1;
        p.strategy_name = "авто + обложка og:image";
        out.push_back(std::move(p));
    }
    // Стратегия 2: заголовок из <title> страницы + текст по density. Полезно,
    // когда h1 на странице — не заголовок статьи, а служебный (хлебные крошки).
    {
        ExtractionProposal p = out.front();
        std::size_t t_open = html.find("<title");
        if (t_open != std::string::npos) {
            const std::size_t t_gt = html.find('>', t_open);
            const std::size_t t_close = html.find("</title", t_gt);
            if (t_gt != std::string::npos && t_close != std::string::npos) {
                const std::string t =
                    html_to_text(html.substr(t_gt + 1, t_close - t_gt - 1));
                if (!t.empty()) p.article.title = t;
            }
        }
        p.strategy = 2;
        p.strategy_name = "заголовок <title> + текст (density)";
        out.push_back(std::move(p));
    }

    // Относительные ссылки на фото (частый случай: "/upload/...") резолвим в
    // абсолютные по базе страницы-источника, чтобы не дописывать домен вручную.
    for (auto& p : out) {
        if (!p.article.image.empty() && p.article.image.find("data:") != 0)
            p.article.image = resolve_url(p.article.image, base_url);
    }

    // Лучшие по «объёму» текста — первыми (чаще всего это и есть статья).
    std::stable_sort(out.begin(), out.end(),
                    [](const ExtractionProposal& a, const ExtractionProposal& b) {
                        return a.score > b.score;
                    });

    // Дедуп по (заголовок+тело), чтобы не предлагать одинаковые варианты.
    std::vector<ExtractionProposal> uniq;
    std::set<std::string> seen;
    for (auto& p : out) {
        const std::string key = p.article.title + "\n" + p.article.body;
        if (seen.insert(key).second) uniq.push_back(std::move(p));
    }
    return uniq;
}

} // namespace news_rewriter
