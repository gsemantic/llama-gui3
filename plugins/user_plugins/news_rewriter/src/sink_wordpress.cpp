#include "sink.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "dotenv.h"
#include "http.h"
#include "json.h"
#include "translit.h"

namespace news_rewriter {

namespace {

// --- helpers ---------------------------------------------------------------

std::string rtrim(const std::string& s, char ch) {
    std::string out = s;
    while (!out.empty() && out.back() == ch) out.pop_back();
    return out;
}

// Экранирование спецсимволов для вставки текста в HTML.
std::string html_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c; break;
        }
    }
    return out;
}

// Простейший Markdown → HTML внутри одного абзаца (до разбиения на <p>).
std::string inline_markdown(const std::string& text) {
    std::string out = html_escape(text);
    // **жирный** → <strong>…</strong>
    std::string bold;
    bool in_bold = false;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '*' && i + 1 < out.size() && out[i + 1] == '*') {
            bold += in_bold ? "</strong>" : "<strong>";
            in_bold = !in_bold;
            ++i;
            continue;
        }
        bold += out[i];
    }
    out = bold;
    // *курсив* → <em>…</em> (одиночная звёздочка, не внутри <strong>)
    std::string em;
    bool in_em = false;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '*') {
            em += in_em ? "</em>" : "<em>";
            in_em = !in_em;
            continue;
        }
        em += out[i];
    }
    return em;
}

// Превращает body_rewritten (текст/лёгкий Markdown) в HTML для WP:
//   - блоки по \n\n → <p>…</p>
//   - одинарные \n внутри блока → <br>
//   - # заголовок → <h2>, ## … ###### подзаголовки → <h3>
std::string body_to_html(const std::string& body) {
    if (body.empty()) return "";

    // Строка-заголовок Markdown: 1–6 решёток + пробел/таб.
    auto heading_line = [](const std::string& line) {
        std::size_t h = 0;
        while (h < line.size() && line[h] == '#') ++h;
        return h >= 1 && h <= 6 && line.size() > h &&
               (line[h] == ' ' || line[h] == '\t');
    };

    // Маркер заголовка бывает приклеен ВНУТРЬ строки абзаца («…дня. ## Как
    // топливный кризис…») — LLM пишет подзаголовок в конец абзаца через
    // пробел. Режем строку на «текст до» и «заголовок до конца строки».
    auto split_inline_heading = [](const std::string& line) {
        std::vector<std::string> out;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] != '#') continue;
            if (i != 0 && line[i - 1] != ' ' && line[i - 1] != '\t') continue;
            std::size_t h = 0;
            while (h < 6 && i + h < line.size() && line[i + h] == '#') ++h;
            if (h >= 1 && i + h < line.size() &&
                (line[i + h] == ' ' || line[i + h] == '\t')) {
                if (i > 0) {
                    std::string left = line.substr(0, i);
                    while (!left.empty() && (left.back() == ' ' ||
                                             left.back() == '\t' ||
                                             left.back() == '\r')) {
                        left.pop_back();
                    }
                    if (!left.empty()) out.push_back(left);
                }
                out.push_back(line.substr(i));
                return out;
            }
        }
        out.push_back(line);
        return out;
    };

    // Нормализация: заголовок должен НАЧИНАТЬ свой блок И ЗАКАНЧИВАТЬ его —
    // иначе склеенный с ним текст уезжает внутрь <h3> (с <br> между строками),
    // а при одиночном \n перед заголовком решётки остаются литералом в абзаце.
    // Вставляем разбивку блока до и после строки-заголовка, если её нет.
    std::string norm;
    norm.reserve(body.size());
    auto tail_has_blank = [](const std::string& s) {
        std::size_t i = s.size();
        while (i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t' ||
                         s[i - 1] == '\r'))
            --i;
        return i >= 2 && s[i - 1] == '\n' && s[i - 2] == '\n';
    };
    {
        std::size_t line_begin = 0;
        while (line_begin < body.size()) {
            std::size_t line_end = body.find('\n', line_begin);
            if (line_end == std::string::npos) line_end = body.size();
            const std::string line =
                body.substr(line_begin, line_end - line_begin);
            for (const std::string& seg : split_inline_heading(line)) {
                if (heading_line(seg)) {
                    if (!norm.empty() && !tail_has_blank(norm)) {
                        std::size_t k = norm.size();
                        while (k > 0 &&
                               (norm[k - 1] == '\n' || norm[k - 1] == ' ' ||
                                norm[k - 1] == '\t' || norm[k - 1] == '\r')) {
                            --k;
                        }
                        norm.resize(k);
                        norm += "\n\n";
                    }
                    norm += seg;
                    norm += "\n\n";
                } else {
                    norm += seg;
                    norm += '\n';
                }
            }
            line_begin = line_end + 1;
        }
    }

    std::vector<std::string> blocks;
    std::string cur;
    for (std::size_t i = 0; i < norm.size(); ++i) {
        if (norm[i] == '\n' && i + 1 < norm.size() && norm[i + 1] == '\n') {
            blocks.push_back(cur);
            cur.clear();
            ++i;
            continue;
        }
        cur += norm[i];
    }
    if (!cur.empty()) blocks.push_back(cur);

    auto strip_stars = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) if (c != '*') out += c;
        return out;
    };

    std::string html;
    bool first_content = true;
    for (auto block : blocks) {
        // Обрезка краевых пробелов/переносов.
        const std::size_t b = block.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        const std::size_t e = block.find_last_not_of(" \t\r\n");
        block = block.substr(b, e - b + 1);

        // Заголовок Markdown: 1–6 решёток + пробел.
        std::size_t h = 0;
        while (h < block.size() && block[h] == '#') h++;
        const bool is_heading = h >= 1 && h <= 6 &&
                                block.size() > h &&
                                (block[h] == ' ' || block[h] == '\t');
        if (is_heading) {
            std::string text = block.substr(h);
            const std::size_t tb = text.find_first_not_of(" \t");
            if (tb != std::string::npos) text = text.substr(tb);
            if (first_content) {
                // Первый блок — это вступление-лид: рендерим жирным абзацем,
                // а не <h2>, даже если модель обернула его в '## ...'.
                html += "<p><strong>" + inline_markdown(strip_stars(text)) +
                        "</strong></p>\n";
            } else {
                // Подзаголовки (## …) — в <h3> для SEO-иерархии (H1 — заголовок
                // записи, H2 резервируем под структуру темы WP).
                html += "<h3>" + inline_markdown(text) + "</h3>\n";
            }
        } else {
            std::string with_br;
            std::string line;
            for (std::size_t i = 0; i < block.size(); ++i) {
                if (block[i] == '\n') {
                    with_br += inline_markdown(line) + "<br>\n";
                    line.clear();
                } else {
                    line += block[i];
                }
            }
            if (!line.empty()) with_br += inline_markdown(line);
            html += "<p>" + with_br + "</p>\n";
        }
        first_content = false;
    }
    return html;
}

// Base64 (RFC 4648) для HTTP Basic авторизации.
std::string base64_encode(const std::string& in) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 2 < in.size()) {
        const uint32_t n = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) |
                           uint8_t(in[i + 2]);
        out += tbl[(n >> 18) & 0x3F];
        out += tbl[(n >> 12) & 0x3F];
        out += tbl[(n >> 6) & 0x3F];
        out += tbl[n & 0x3F];
        i += 3;
    }
    const std::size_t rem = in.size() - i;
    if (rem == 1) {
        const uint32_t n = uint8_t(in[i]) << 16;
        out += tbl[(n >> 18) & 0x3F];
        out += tbl[(n >> 12) & 0x3F];
        out += "==";
    } else if (rem == 2) {
        const uint32_t n = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8);
        out += tbl[(n >> 18) & 0x3F];
        out += tbl[(n >> 12) & 0x3F];
        out += tbl[(n >> 6) & 0x3F];
        out += "=";
    }
    return out;
}

std::string mime_from_url(const std::string& url) {
    const std::size_t q = url.find('?');
    const std::string path = (q == std::string::npos) ? url : url.substr(0, q);
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "application/octet-stream";
    const std::string ext = path.substr(dot + 1);
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "svg") return "image/svg+xml";
    return "application/octet-stream";
}

std::string filename_from_url(const std::string& url) {
    const std::size_t q = url.find('?');
    const std::string path = (q == std::string::npos) ? url : url.substr(0, q);
    const std::size_t slash = path.find_last_of('/');
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    if (name.empty()) name = "image";
    return name;
}

// Распознаёт тип картинки по «магическим» байтам (сигнатурам формата), чтобы
// WordPress получал корректный Content-Type и расширение даже для CDN-URL БЕЗ
// расширения. Напр. Дзен/Яндекс отдают картинки по opaque-ссылкам
// (avatars.mds.yandex.net/...), и mime_from_url() выдаёт
// application/octet-stream — WP REST /media такое отвергает с HTTP 500, из-за
// чего картинка не попадает в медиатеку и остаётся ссылкой на исходник.
std::string detect_image_mime(const std::string& data) {
    const std::size_t n = data.size();
    if (n >= 3 &&
        static_cast<unsigned char>(data[0]) == 0xFF &&
        static_cast<unsigned char>(data[1]) == 0xD8 &&
        static_cast<unsigned char>(data[2]) == 0xFF) {
        return "image/jpeg";
    }
    if (n >= 8 && data[0] == '\x89' && data[1] == 'P' && data[2] == 'N' &&
        data[3] == 'G' && data[4] == '\r' && data[5] == '\n' &&
        data[6] == '\x1A' && data[7] == '\n') {
        return "image/png";
    }
    if (n >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F' &&
        data[3] == '8' && (data[4] == '7' || data[4] == '9') && data[5] == 'a') {
        return "image/gif";
    }
    if (n >= 12 && data.compare(0, 4, "RIFF") == 0 &&
        data.compare(8, 4, "WEBP") == 0) {
        return "image/webp";
    }
    if (n >= 2 && data[0] == 'B' && data[1] == 'M') return "image/bmp";
    return "";
}

std::string ext_for_mime(const std::string& mime) {
    if (mime == "image/jpeg") return "jpg";
    if (mime == "image/png") return "png";
    if (mime == "image/gif") return "gif";
    if (mime == "image/webp") return "webp";
    if (mime == "image/bmp") return "bmp";
    return "";
}

// Процентное URL-кодирование (для параметра ?search= в WP REST).
std::string url_encode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[(c >> 4) & 0x0F];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

// Регистронезависимое сравнение строк (ASCII; кириллица сравнивается как есть —
// WP отдаёт имена в точном регистре, поэтому для кириллицы важно совпадение).
bool ci_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + ('a' - 'A'));
        if (ca != cb) return false;
    }
    return true;
}

// Трим краёв (без схлопывания внутренних пробелов).
std::string trim_edge(const std::string& s) {
    const std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const std::size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Разбивает путь рубрики на уровни иерархии по разделителям
// " > ", ">", "/", "|", "»", "→". Возвращает нетронутый список, если разделителей
// нет (одноуровневая рубрика).
//
// Разделители "»" (0xC2 0xBB) и "→" (0xE2 0x86 0x92) — многобайтовые в UTF-8,
// поэтому сравниваем их как последовательности байт, а не как char-литералы.
std::vector<std::string> split_taxonomy_path(const std::string& path) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        std::string t = trim_edge(cur);
        if (!t.empty()) out.push_back(t);
        cur.clear();
    };
    const std::size_t n = path.size();
    for (std::size_t i = 0; i < n; ++i) {
        unsigned char c = static_cast<unsigned char>(path[i]);
        bool sep = false;
        std::size_t adv = 1;
        if (c == '>' || c == '/' || c == '|') {
            sep = true;
        } else if (c == 0xC2 && i + 1 < n &&
                   static_cast<unsigned char>(path[i + 1]) == 0xBB) {
            // "»"
            sep = true;
            adv = 2;
        } else if (c == 0xE2 && i + 2 < n &&
                   static_cast<unsigned char>(path[i + 1]) == 0x86 &&
                   static_cast<unsigned char>(path[i + 2]) == 0x92) {
            // "→"
            sep = true;
            adv = 3;
        }
        if (sep) {
            flush();
            i += adv - 1;
        } else {
            cur += static_cast<char>(c);
        }
    }
    flush();
    return out;
}

// Удаляет дубликаты из списка int, сохраняя порядок первого появления.
std::vector<int> dedupe_ints(std::vector<int> v) {
    std::vector<int> out;
    for (int x : v) {
        bool found = false;
        for (int y : out) if (y == x) { found = true; break; }
        if (!found) out.push_back(x);
    }
    return out;
}

// Похожий материал на выходном сайте (для внутренней перелинковки).
struct RelatedPost {
    std::string title;   // очищенный от тегов заголовок
    std::string link;    // абсолютный URL записи
};

// Убирает HTML-теги из строки (заголовки WP приходят как HTML).
std::string strip_html(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    bool in_tag = false;
    for (char c : in) {
        if (c == '<') { in_tag = true; continue; }
        if (c == '>') { in_tag = false; continue; }
        if (!in_tag) out += c;
    }
    // Схлопываем повторные пробелы и краевые.
    std::string norm;
    bool space = false;
    for (char c : out) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!space) norm += ' ';
            space = true;
        } else {
            norm += c;
            space = false;
        }
    }
    const std::size_t b = norm.find_first_not_of(' ');
    if (b == std::string::npos) return "";
    const std::size_t e = norm.find_last_not_of(' ');
    return norm.substr(b, e - b + 1);
}

// Собирает HTML-блок «Источники» из внешних ссылок оригинала.
// Все ссылки открываются в новой вкладке (target="_blank", rel="noopener").
std::string build_external_links_html(const std::vector<ExternalLink>& links) {
    if (links.empty()) return "";
    std::string html = "<p><strong>Источники:</strong></p>\n<ul>\n";
    for (const auto& l : links) {
        const std::string text = l.text.empty() ? html_escape(host_of(l.url)) : html_escape(l.text);
        html += "<li><a href=\"" + html_escape(l.url) +
                "\" target=\"_blank\" rel=\"noopener\">" + text + "</a></li>\n";
    }
    html += "</ul>\n";
    return html;
}

// Собирает HTML-блок «Читайте также» из похожих материалов выходного сайта.
// Ссылки открываются в новой вкладке (target="_blank", rel="noopener").
std::string build_related_html(const std::vector<RelatedPost>& related) {
    if (related.empty()) return "";
    std::string html = "<p><strong>Читайте также:</strong></p>\n<ul>\n";
    for (const auto& r : related) {
        const std::string text = r.title.empty() ? html_escape(r.link) : html_escape(r.title);
        html += "<li><a href=\"" + html_escape(r.link) +
                "\" target=\"_blank\" rel=\"noopener\">" + text + "</a></li>\n";
    }
    html += "</ul>\n";
    return html;
}

// --- Первоисточник и инлайн-перелинковка ------------------------------------

// Нижний регистр для ASCII и кириллицы (UTF-8). Длина строки в байтах не
// меняется, поэтому смещения в приведённой копии совпадают с оригиналом.
std::string lower_ru(const std::string& in) {
    std::string s = in;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 'A' && c <= 'Z') s[i] = static_cast<char>(c - 'A' + 'a');
    }
    for (std::size_t i = 0; i + 1 < s.size(); ++i) {
        const unsigned char c0 = static_cast<unsigned char>(s[i]);
        const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
        if (c0 == 0xD0 && c1 >= 0x90 && c1 <= 0x9F) {
            s[i + 1] = static_cast<char>(c1 + 0x20);   // А..П → а..п
        } else if (c0 == 0xD0 && c1 >= 0xA0 && c1 <= 0xAF) {
            s[i] = static_cast<char>(0xD1);            // Р..Я → р..я
            s[i + 1] = static_cast<char>(c1 - 0x20);
        } else if (c0 == 0xD0 && c1 == 0x81) {
            s[i] = static_cast<char>(0xD1);            // Ё → ё
            s[i + 1] = static_cast<char>(0x91);
        }
    }
    return s;
}

// Первые nchars символов UTF-8 строки.
std::string utf8_prefix(const std::string& s, std::size_t nchars) {
    std::size_t i = 0, chars = 0;
    while (i < s.size() && chars < nchars) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        if ((c & 0x80) != 0)
            len = ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : 4;
        i += len;
        ++chars;
    }
    return s.substr(0, i);
}

// Анкор намекает на ссылку-источник («по данным …», «источник», «сообщает»).
bool anchor_mentions_source(const std::string& text) {
    static const char* kW[] = {"источник", "по данным", "по информации",
                               "сообщает", "source", "reports"};
    const std::string t = lower_ru(text);
    for (const char* w : kW)
        if (!std::string(w).empty() && t.find(w) != std::string::npos) return true;
    return false;
}

// Выбирает ОДНУ ссылку на первоисточник из внешних ссылок оригинала:
// 1) nofollow/noindex + говорящий анкор; 2) nofollow/noindex; 3) анкор.
const ExternalLink* pick_primary_source(const std::vector<ExternalLink>& links) {
    const ExternalLink* best = nullptr;
    int best_rank = 0;
    for (const auto& l : links) {
        int rank = 0;
        if (l.source_ref && anchor_mentions_source(l.text)) rank = 3;
        else if (l.source_ref) rank = 2;
        else if (anchor_mentions_source(l.text)) rank = 1;
        if (rank > best_rank) { best = &l; best_rank = rank; }
    }
    return best;
}

// Слова значимой длины из заголовка (нижний регистр): кандидаты на якорь.
std::vector<std::string> title_keywords(const std::string& title) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : lower_ru(title)) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool letter = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                            c >= 0x80;
        if (letter) cur += ch;
        else { if (cur.size() >= 8) out.push_back(cur); cur.clear(); }
    }
    if (cur.size() >= 8) out.push_back(cur);
    return out;
}

struct ProseWord { std::size_t begin; std::size_t end; };

// Слова в текстовых узлах HTML: вне тегов, вне <a>…</a> и заголовков h1–h6
// (в них ссылки не встраиваем). Смещения — по исходной строке.
std::vector<ProseWord> prose_words(const std::string& html) {
    std::vector<ProseWord> out;
    int anchor_depth = 0, heading_depth = 0;
    std::size_t word_begin = std::string::npos;
    auto flush = [&](std::size_t i) {
        if (word_begin != std::string::npos) {
            if (anchor_depth == 0 && heading_depth == 0 && i > word_begin)
                out.push_back({word_begin, i});
            word_begin = std::string::npos;
        }
    };
    auto is_heading = [](const std::string& n) {
        return n.size() == 2 && n[0] == 'h' && n[1] >= '1' && n[1] <= '6';
    };
    std::size_t i = 0;
    while (i < html.size()) {
        if (html[i] == '<') {
            flush(i);
            std::size_t j = i + 1;
            bool closing = false;
            if (j < html.size() && html[j] == '/') { closing = true; ++j; }
            std::string name;
            while (j < html.size()) {
                const char ch = html[j];
                const unsigned char uc = static_cast<unsigned char>(ch);
                const bool name_ch =
                    (uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z') ||
                    (uc >= '0' && uc <= '9');
                if (!name_ch) break;
                name += static_cast<char>(
                    (uc >= 'A' && uc <= 'Z') ? uc - 'A' + 'a' : uc);
                ++j;
            }
            while (j < html.size() && html[j] != '>') ++j;
            if (j < html.size()) ++j;
            if (name == "a") {
                anchor_depth += closing ? -1 : 1;
                if (anchor_depth < 0) anchor_depth = 0;
            } else if (is_heading(name)) {
                heading_depth += closing ? -1 : 1;
                if (heading_depth < 0) heading_depth = 0;
            }
            i = j;
            continue;
        }
        const unsigned char uc = static_cast<unsigned char>(html[i]);
        const bool letter = (uc >= 'a' && uc <= 'z') || (uc >= '0' && uc <= '9') ||
                            uc >= 0x80;
        if (letter) {
            if (word_begin == std::string::npos) word_begin = i;
        } else {
            flush(i);
        }
        ++i;
    }
    flush(html.size());
    return out;
}

// Вставляет <a href> вокруг пары соседних слов текста, совпадающей с парой
// ключевых слов заголовка похожей записи (сравнение по 5-символьному префиксу
// переживает падежные окончания; окно длины отсекает однокоренные «не те»
// слова вроде «министр» против «министерство»). Возвращает новый HTML либо
// пустую строку, если подходящего места нет.
std::string insert_inline_link(const std::string& html, const std::string& url,
                               const std::vector<std::string>& kws,
                               const std::set<std::string>& used_urls) {
    if (kws.size() < 2 || url.empty() || used_urls.count(url)) return "";
    const std::string low = lower_ru(html);
    const std::vector<ProseWord> words = prose_words(low);
    if (words.size() < 2) return "";
    for (std::size_t k = 0; k + 1 < kws.size(); ++k) {
        // Префикс 4 символа: у прилагательных падеж меняет уже 5-ю букву
        // (скорая/скорой/скорую), пара слов отсекает ложные совпадения.
        const std::string p1 = utf8_prefix(kws[k], 4);
        const std::string p2 = utf8_prefix(kws[k + 1], 4);
        if (p1.size() < 8 || p2.size() < 8) continue;
        for (std::size_t w = 0; w + 1 < words.size(); ++w) {
            const std::string t1 = low.substr(words[w].begin,
                                              words[w].end - words[w].begin);
            const std::string t2 = low.substr(words[w + 1].begin,
                                              words[w + 1].end - words[w + 1].begin);
            if (utf8_prefix(t1, 4) != p1 || utf8_prefix(t2, 4) != p2) continue;
            // Окно длины: слово текста не короче ключевого на >1 символ и не
            // длиннее на >4 (морфологические суффиксы).
            if (t1.size() + 2 < kws[k].size() ||
                t1.size() > kws[k].size() + 8) continue;
            if (t2.size() + 2 < kws[k + 1].size() ||
                t2.size() > kws[k + 1].size() + 8) continue;
            const std::size_t from = words[w].begin;
            const std::size_t to = words[w + 1].end;
            std::string out = html.substr(0, from);
            out += "<a href=\"" + html_escape(url) +
                   "\" target=\"_blank\" rel=\"noopener\">";
            out += html.substr(from, to - from);
            out += "</a>";
            out += html.substr(to);
            return out;
        }
    }
    return "";
}

// --- WordPressSink ----------------------------------------------------------

class WordPressSink : public Sink {
public:
    WordPressSink(const SinkConfig& cfg, Storage& storage, const LogFn& log)
        : env_path_(!cfg.data_dir.empty()
                       ? cfg.data_dir + "/news_rewriter/.env"
                       : (storage.root().empty() ? std::string(".env")
                                                 : storage.root() + "/.env")),
          storage_(storage),
          verify_site_state_(
              cfg.params.get("verify_site_state").as_bool(true)),
          site_url_(rtrim(cfg.params.get("site_url").as_string(), '/')),
          username_(cfg.params.get("username").as_string()),
          app_password_(cfg.params.get("app_password").as_string()),
          status_(cfg.params.get("status").as_string("draft")),
          post_type_(cfg.params.get("post_type").as_string("posts")),
          excerpt_(cfg.params.get("excerpt").as_string()),
          slug_(cfg.params.get("slug").as_string()),
          featured_image_(cfg.params.get("featured_image").as_string()),
          taxonomy_auto_assign_(
              cfg.params.get("taxonomy_auto_assign").as_bool(true)),
          internal_related_max_(
              static_cast<int>(cfg.params.get("internal_related_max").as_int(2))),
          external_links_mode_(
              cfg.params.get("external_links_mode").as_string("source")),
          timeout_(static_cast<int>(
              cfg.params.get("timeout_seconds").as_int(20))),
          max_retries_(static_cast<int>(
              cfg.params.get("max_retries").as_int(0))),
          retry_delay_ms_(static_cast<int>(
              cfg.params.get("retry_delay_ms").as_int(1000))),
          log_(log) {
        // Опциональные числовые id-массивы/скаляры.
        const Json& cats = cfg.params.get("categories");
        if (cats.is_array()) {
            for (std::size_t i = 0; i < cats.size(); ++i)
                categories_.push_back(static_cast<int>(cats[i].as_int(0)));
        }
        const Json& tags = cfg.params.get("tags");
        if (tags.is_array()) {
            for (std::size_t i = 0; i < tags.size(); ++i)
                tags_.push_back(static_cast<int>(tags[i].as_int(0)));
        }
        if (cfg.params.get("author").is_number())
            author_ = static_cast<int>(cfg.params.get("author").as_int(0));
        // Нормализация app_password: убираем пробелы перед кодированием.
        std::string norm;
        for (char c : app_password_) if (c != ' ') norm += c;
        app_password_ = norm;
    }

    bool write(const Article& article) override {
        // Учётные данные: из .env (приоритет), иначе из params (fallback/тесты).
        std::string user = dotenv_read(env_path_, kNewsRewriterWpUser);
        if (user.empty()) user = username_;
        std::string pass = dotenv_read(env_path_, kNewsRewriterWpPass);
        if (pass.empty()) pass = app_password_;
        {
            std::string norm;
            for (char c : pass) if (c != ' ') norm += c;
            pass = norm;
        }
        if (site_url_.empty() || user.empty() || pass.empty()) {
            if (log_) {
                log_("WordPressSink: не заданы site_url/логин/пароль "
                     "(проверьте " + env_path_ + " и параметры sink)");
            }
            return false;
        }
        if (article.title_rewritten.empty() || article.body_rewritten.empty()) {
            if (log_) log_("WordPressSink: пустой рерайт (title/body), пропуск");
            return false;
        }
        if (!client_.init()) {
            if (log_) log_("WordPressSink: libcurl недоступен");
            return false;
        }

        NetworkConfig nc;
        nc.timeout_seconds = timeout_;
        const std::string endpoint = site_url_ + "/wp-json/wp/v2/" + post_type_;
        const std::string auth = base64_encode(user + ":" + pass);
        // Заглавное изображение: ручной URL из параметров sink либо авто —
        // картинка из источника (Article.source_image). Резолвим в абсолютный,
        // т.к. в источнике оно часто относительное.
        std::string image_url =
            featured_image_.empty()
                ? resolve_url(article.source_image, article.url)
                : featured_image_;

        // Заливаем картинку в медиабиблиотеку WP ДО создания поста, чтобы и
        // обложка, и встроенное в текст фото ссылались на картинку с хостинга
        // WP, а не на исходник источника. Если заливка не удалась — откатываемся
        // на прямую ссылку на исходник (чтобы пост всё равно нес фото).
        const std::string alt = article.seo_focus_keyword.empty()
                                    ? article.title_rewritten
                                    : article.seo_focus_keyword;
        int media_id = 0;
        std::string media_url;
        if (!image_url.empty()) {
            const auto m = upload_media(image_url, nc, user, pass, alt,
                                        article.url);
            media_id = m.first;
            media_url = m.second;
        }

        // Тело статьи в HTML. Hero-картинку вставляем в текст ТОЛЬКО если не
        // удалось залить её в медиатеку WP (media_id == 0): тогда обложка
        // (featured_media) не задана и пост бы остался без фото. Если заливка
        // прошла — тема уже показывает изображение через featured_media, и
        // дублировать его внутри текста (в оригинальном размере) не нужно.
        // --- Таксономия: резолвим рубрики/теги ДО сборки HTML, чтобы по
        // сгенерированным тегам можно было найти похожие материалы на сайте. ---
        std::vector<int> cat_ids, tag_ids;
        if (post_type_ != "pages") {
            cat_ids = categories_;
            tag_ids = tags_;
            // Динамическая таксономия из статьи (переведённые рубрики/теги).
            if (taxonomy_auto_assign_) {
                const std::vector<int> dyn_cats =
                    resolve_categories(article, nc, auth);
                const std::vector<int> dyn_tags =
                    resolve_tags(article, nc, auth);
                cat_ids.insert(cat_ids.end(), dyn_cats.begin(), dyn_cats.end());
                tag_ids.insert(tag_ids.end(), dyn_tags.begin(), dyn_tags.end());
            }
            // Убираем дефолтную рубрику WP («Без рубрики»), чтобы не дублировать
            // её с нашими назначениями — иначе пост попадает и в подходящую
            // рубрику, и в «Без рубрики» одновременно. GET за списком рубрик
            // делаем только когда есть что фильтровать (иначе лишний запрос).
            int def = 0;
            if (!cat_ids.empty()) {
                def = default_category_id(nc, auth);
                if (def != 0) {
                    std::vector<int> filtered;
                    for (int id : cat_ids) if (id != def) filtered.push_back(id);
                    cat_ids = std::move(filtered);
                }
            }
            // Fallback: если конкретных рубрик нет, но таксономия включена И
            // сайт настроен на рубрики (заданы в конфиге или выведены LLM) —
            // назначаем рубрику по имени источника, иначе WP всё равно поставит
            // «Без рубрики». Не делаем лишний запрос, когда рубрик нет вообще
            // (пост и так попадёт в «Без рубрики» — это ожидаемое поведение).
            if (taxonomy_auto_assign_ && cat_ids.empty() &&
                !article.source.empty() &&
                (!categories_.empty() || !article.categories_ru.empty())) {
                const int fid = resolve_term("categories", article.source, 0, nc, auth);
                if (fid != 0 && fid != def) cat_ids.push_back(fid);
            }
            cat_ids = dedupe_ints(cat_ids);
            tag_ids = dedupe_ints(tag_ids);
        }

        // Внутренние «похожие материалы» выходного сайта (по тегам статьи или
        // ключевому слову). 1–2 ссылки; если подходящих нет — список пустой,
        // блок «Читайте также» не добавляется.
        std::vector<RelatedPost> related;
        if (article.link_internal_related && post_type_ != "pages") {
            int maxrel = internal_related_max_;
            if (maxrel < 1) maxrel = 1;
            if (maxrel > 2) maxrel = 2;
            related = find_related_posts(nc, auth, tag_ids, maxrel,
                                         article.seo_focus_keyword);
            if (log_ && !related.empty()) {
                log_("WordPressSink: добавлены внутренние ссылки на похожие "
                     "материалы: " + std::to_string(related.size()));
            }
        }

        std::string html = body_to_html(article.body_rewritten);
        if (media_id == 0) {
            const std::string hero_src = media_url.empty() ? image_url : media_url;
            if (!hero_src.empty()) {
                html = "<p><img src=\"" + html_escape(hero_src) + "\" alt=\"" +
                       html_escape(alt) + "\"></p>\n" + html;
            }
        }
        // Внешние ссылки оригинала. По умолчанию (source) — только ОДНА
        // ссылка на первоисточник одной строкой: публиковать все внешние
        // ссылки исходной статьи не нужно (получался «список из 23 ссылок»).
        // external_links_mode=all вернёт прежний блок «Источники», none —
        // полностью отключит вывод.
        if (external_links_mode_ == "all") {
            html += build_external_links_html(article.external_links);
        } else if (external_links_mode_ == "source") {
            const ExternalLink* primary =
                pick_primary_source(article.external_links);
            if (primary) {
                html += "\n<p>Первоисточник: <a href=\"" +
                        html_escape(primary->url) +
                        "\" target=\"_blank\" rel=\"noopener nofollow\">" +
                        html_escape(host_of(primary->url)) + "</a></p>";
                if (log_)
                    log_("WordPressSink: первоисточник оригинала: " +
                         primary->url);
            }
        }
        // Внутренняя перелинковка: сначала пробуем встроить ссылки прямо в
        // текст — на упоминании темы похожей записи (по словам заголовка).
        // Куда встроить не удалось — остаются в блоке «Читайте также».
        {
            std::vector<RelatedPost> rest;
            std::set<std::string> used_urls;
            int placed_inline = 0;
            for (const auto& r : related) {
                const std::vector<std::string> kws = title_keywords(r.title);
                std::string updated =
                    insert_inline_link(html, r.link, kws, used_urls);
                if (!updated.empty()) {
                    html = std::move(updated);
                    used_urls.insert(r.link);
                    ++placed_inline;
                } else {
                    rest.push_back(r);
                }
            }
            if (log_ && !related.empty()) {
                log_("WordPressSink: внутренние ссылки: в тексте=" +
                     std::to_string(placed_inline) + ", в блоке=" +
                     std::to_string(rest.size()));
            }
            // Внутренние похожие материалы (блок «Читайте также»).
            html += build_related_html(rest);
        }
        {
            const std::string host = host_of(article.url);
            html += "\n<p>Источник: <a href=\"" + html_escape(article.url) +
                    "\">" + html_escape(host) + "</a></p>";
        }
        // Примечание: подпись «Автор оригинала» теперь формируется самим LLM в
        // теле рерайта (с кириллической транслитерацией имени в скобках), поэтому
        // здесь её не дублируем — иначе будет две разные строки автора.
        if (article.published_at > 0) {
            html += "<p>Дата оригинала: " +
                    html_escape(iso8601_of(article.published_at)) + "</p>";
        }

        Json body = Json::object();
        body["title"] = article.title_rewritten;
        body["content"] = html;
        body["status"] = status_;
        if (!excerpt_.empty()) body["excerpt"] = excerpt_;
        // Slug: ручной из params → SEO-slug из focus_keyword → транслит
        // focus_keyword на месте (SEO-шаг мог не отдать slug) → стабильный
        // slug от источника (host_hash, нужен дедупу по сайту).
        body["slug"] = candidate_slugs(article).front();
        // Обложка поста — id картинки, залитой в медиатеку WP выше.
        if (media_id != 0) body["featured_media"] = static_cast<int64_t>(media_id);
        // Категории/теги поддерживают не все типы: стандартные «страницы»
        // (pages) их не принимают — WP вернёт 400. Для них не шлём таксономию.
        if (post_type_ != "pages") {
            if (!cat_ids.empty()) {
                Json arr = Json::array();
                for (int id : cat_ids) arr.push(static_cast<int64_t>(id));
                body["categories"] = arr;
            }
            if (!tag_ids.empty()) {
                Json arr = Json::array();
                for (int id : tag_ids) arr.push(static_cast<int64_t>(id));
                body["tags"] = arr;
            }
        }
        if (author_ != 0) body["author"] = static_cast<int64_t>(author_);

        // Авто-SEO: прокидываем наши собственные мета-ключи nr_seo_*. Читаются
        // тонким MU-плагином nr-seo.php (см. mu-plugins/nr-seo.php) и выводятся
        // в <head> — замена тяжёлым SEO-плагинам вроде Yoast/RankMath. Поля
        // заполняются рерайтером только если в конфиге включен seo.enabled.
        {
            Json meta = Json::object();
            if (!article.seo_focus_keyword.empty())
                meta["nr_seo_keyword"] = article.seo_focus_keyword;
            if (!article.seo_meta_description.empty())
                meta["nr_seo_description"] = article.seo_meta_description;
            if (!article.seo_title.empty())
                meta["nr_seo_title"] = article.seo_title;
            if (!meta.empty()) body["meta"] = meta;
        }

        // Excerpt из SEO-meta, если не задан вручную в параметрах sink.
        if (excerpt_.empty() && !article.seo_meta_description.empty()) {
            body["excerpt"] = article.seo_meta_description;
        }

        std::vector<std::string> headers = {
            "Content-Type: application/json",
            "Authorization: Basic " + auth};

        const std::string payload = body.dump();
        int attempt = 0;
        HttpResponse resp;
        for (;;) {
            resp = client_.post(endpoint, payload, nc, headers);
            if (resp.ok && resp.status == 201) break;
            if (log_) {
                log_("WordPressSink: создание поста не удалось (HTTP " +
                     std::to_string(resp.status) + "): " +
                     (resp.error.empty() ? resp.body : resp.error));
            }
            if (attempt >= max_retries_) return false;
            if (retry_delay_ms_ > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(retry_delay_ms_));
            }
            attempt++;
        }

        const int post_id = parse_post_id(resp.body);
        if (log_) {
            log_("WordPressSink: объект «" + post_type_ + "» создан id=" +
                 std::to_string(post_id) + " (HTTP 201) для " + article.url);
        }

        // Обложка (featured_media) уже залита в медиатеку WP выше и привязана к
        // посту через поле featured_media в теле запроса. Повторная заливка не
        // нужна.
        return true;
    }

    // Все варианты slug, которыми пост публикуется/мог быть опубликован
    // (в порядке приоритета записи). Используется и для записи, и для
    // сверки дедупа с сайтом (пост мог выйти с любым из исторических slug).
    std::vector<std::string> candidate_slugs(const Article& a) const {
        std::vector<std::string> out;
        if (!slug_.empty()) {
            out.push_back(slug_);
            return out;
        }
        if (!a.seo_slug.empty()) out.push_back(a.seo_slug);
        if (!a.seo_focus_keyword.empty()) {
            const std::string ks = make_slug(a.seo_focus_keyword);
            if (!ks.empty()) out.push_back(ks);
        }
        out.push_back(storage_.slug_for(a.url));
        return out;
    }

    const char* name() const override { return "wordpress"; }

    // Реальное состояние статьи на сайте: есть ли уже пост с нашим slug-ом.
    // Возвращает Present/Absent (при успешном запросе) либо Unknown, если
    // сверка отключена или сайт недоступен/нет прав.
    Presence presence(const Article& a) const override {
        if (!verify_site_state_) return Presence::Unknown;
        std::string user, pass;
        if (!resolve_credentials(user, pass)) return Presence::Unknown;

        HttpClient client;
        if (!client.init()) return Presence::Unknown;
        NetworkConfig nc;
        nc.timeout_seconds = timeout_;
        const std::string auth = "Authorization: Basic " +
                                 base64_encode(user + ":" + pass);
        // Пост мог быть опубликован с любым из исторических slug
        // (SEO-slug, транслит ключевого слова, host_hash) — проверяем все.
        for (const std::string& slug : candidate_slugs(a)) {
            const std::string ep = site_url_ + "/wp-json/wp/v2/" + post_type_ +
                                   "?slug=" + slug + "&status=any&per_page=1";
            HttpResponse resp =
                client.get(ep, nc, std::vector<std::string>{auth});
            if (!resp.ok || resp.status != 200) {
                if (log_) {
                    log_("WordPressSink: не удалось сверить состояние сайта "
                         "(HTTP " + std::to_string(resp.status) +
                         "), дедуп по локальному индексу");
                }
                return Presence::Unknown;
            }
            bool ok = false;
            Json j = Json::parse(resp.body, &ok);
            if (!ok || !j.is_array()) return Presence::Unknown;
            if (j.size() > 0) return Presence::Present;
        }
        return Presence::Absent;
    }

private:
    // Считывает учётные данные (.env приоритет, иначе params) и нормализует
    // app_password. Возвращает false, если заданы не все данные.
    bool resolve_credentials(std::string& user, std::string& pass) const {
        user = dotenv_read(env_path_, kNewsRewriterWpUser);
        if (user.empty()) user = username_;
        pass = dotenv_read(env_path_, kNewsRewriterWpPass);
        if (pass.empty()) pass = app_password_;
        auto norm = [](std::string s) {
            std::string out;
            for (char c : s) if (c != ' ') out += c;
            return out;
        };
        pass = norm(pass);
        return !site_url_.empty() && !user.empty() && !pass.empty();
    }

    static int parse_post_id(const std::string& body) {        bool ok = false;
        Json j = Json::parse(body, &ok);
        if (!ok) return 0;
        const Json& id = j["id"];
        if (id.is_number()) return static_cast<int>(id.as_int(0));
        return 0;
    }

    // Загружает картинку по URL в медиабиблиотеку WP и возвращает
    // (media_id, source_url). source_url — это уже адрес картинки на хостинге
    // WP (а не исходник источника), его и нужно вставлять в текст поста и
    // использовать как обложку. alt — alt-текст обложки (обычно ключевое слово
    // модели из SEO-шага).
    std::pair<int, std::string> upload_media(const std::string& image_url,
                                              const NetworkConfig& nc,
                                              const std::string& user,
                                              const std::string& pass,
                                              const std::string& alt = "",
                                              const std::string& referer = "") {
        std::pair<int, std::string> none{0, ""};
        // CDN (Дзен/Яндекс) нередко требуют Referer, иначе отдают заглушку
        // вместо картинки — заливка такого «файла» в WP падает с 500.
        std::vector<std::string> dl_headers;
        if (!referer.empty()) dl_headers.push_back("Referer: " + referer);
        HttpResponse img = client_.get(image_url, nc, dl_headers);
        if (!img.ok || img.body.empty()) {
            if (log_) log_("WordPressSink: не удалось скачать картинку " + image_url);
            return none;
        }
        // Тип берём по РЕАЛЬНЫМ байтам файла, а не по расширению в URL (CDN
        // часто без расширения → application/octet-stream → WP отвергает → 500).
        std::string mime = detect_image_mime(img.body);
        if (mime.empty()) mime = mime_from_url(image_url);
        std::string fname = filename_from_url(image_url);
        const std::string ext = ext_for_mime(mime);
        if (!ext.empty()) {
            const std::size_t dot = fname.find_last_of('.');
            if (dot == std::string::npos) fname += "." + ext;
            else fname = fname.substr(0, dot + 1) + ext;
        }
        const std::string media_ep = site_url_ + "/wp-json/wp/v2/media";
        const std::string auth = base64_encode(user + ":" + pass);

        std::vector<std::string> headers = {
            "Content-Type: " + mime,
            "Content-Disposition: attachment; filename=\"" + fname + "\"",
            "Authorization: Basic " + auth};

        HttpResponse media_resp = client_.post(media_ep, img.body, nc, headers);
        if (!media_resp.ok || media_resp.status != 201) {
            if (log_) {
                std::string detail = media_resp.body;
                if (detail.size() > 300) detail = detail.substr(0, 300) + "…";
                log_("WordPressSink: загрузка медиа не удалась (HTTP " +
                     std::to_string(media_resp.status) + ")" +
                     (detail.empty() ? std::string("") : ": " + detail));
            }
            return none;
        }
        const int media_id = parse_post_id(media_resp.body);
        if (media_id == 0) return none;

        // Адрес картинки на хостинге WP (именно его подставляем в текст/обложку).
        bool ok = false;
        Json mj = Json::parse(media_resp.body, &ok);
        std::string source_url = ok ? mj.get("source_url").as_string() : std::string();

        // alt-текст обложки (ключевое слово модели из SEO-шага).
        if (!alt.empty() && !source_url.empty()) {
            Json altpatch = Json::object();
            altpatch["alt_text"] = alt;
            std::vector<std::string> ah = {
                "Content-Type: application/json",
                "Authorization: Basic " + auth};
            client_.post(media_ep + "/" + std::to_string(media_id),
                         altpatch.dump(), nc, ah);
        }
        if (log_) {
            log_("WordPressSink: медиа загружено media_id=" +
                 std::to_string(media_id) + " (" + source_url + ") для " +
                 image_url);
        }
        return {media_id, source_url};
    }

    // Резолвит термин таксономии (categories/tags) по имени: ищет существующий
    // (с точным совпадением, с учётом parent для categories), иначе создаёт
    // через WP REST API. Возвращает id (0 при неудаче, best-effort — пост не
    // должен падать из-за таксономии). Для tags parent игнорируется (теги
    // плоские).
    int resolve_term(const std::string& taxonomy, const std::string& raw_name,
                     int parent_id, const NetworkConfig& nc,
                     const std::string& auth) {
        std::string name = trim_edge(raw_name);
        if (name.empty()) return 0;
        if (taxonomy != "categories" && taxonomy != "tags") return 0;

        // 1) Ищем существующий термин.
        const std::string search_ep =
            site_url_ + "/wp-json/wp/v2/" + taxonomy + "?search=" +
            url_encode(name) +
            (taxonomy == "categories"
                 ? "&parent=" + std::to_string(parent_id)
                 : "");
        HttpResponse sr = client_.get(
            search_ep, nc,
            std::vector<std::string>{"Authorization: Basic " + auth});
        if (sr.ok && sr.status == 200) {
            bool ok = false;
            Json arr = Json::parse(sr.body, &ok);
            if (ok && arr.is_array()) {
                for (std::size_t i = 0; i < arr.size(); ++i) {
                    const Json& item = arr[i];
                    if (!item.is_object()) continue;
                    if (ci_equal(item.get("name").as_string(), name)) {
                        const Json& idj = item.get("id");
                        if (idj.is_number())
                            return static_cast<int>(idj.as_int(0));
                    }
                }
            }
        }

        // 2) Не нашли — создаём.
        Json payload = Json::object();
        payload["name"] = name;
        if (taxonomy == "categories" && parent_id != 0)
            payload["parent"] = static_cast<int64_t>(parent_id);
        const std::string ep = site_url_ + "/wp-json/wp/v2/" + taxonomy;
        std::vector<std::string> headers = {
            "Content-Type: application/json",
            "Authorization: Basic " + auth};
        HttpResponse cr = client_.post(ep, payload.dump(), nc, headers);
        if (cr.ok && (cr.status == 201 || cr.status == 200)) {
            bool ok = false;
            Json j = Json::parse(cr.body, &ok);
            if (ok && j.is_object()) {
                const Json& idj = j.get("id");
                if (idj.is_number()) {
                    const int id = static_cast<int>(idj.as_int(0));
                    if (log_) {
                        log_("WordPressSink: термин «" + name + "» (" +
                             taxonomy + ") создан id=" + std::to_string(id));
                    }
                    return id;
                }
            }
        }
        if (log_) {
            std::string detail = cr.body;
            if (detail.size() > 200) detail = detail.substr(0, 200) + "…";
            log_("WordPressSink: не удалось резолвить термин «" + name +
                 "» (" + taxonomy + "): HTTP " + std::to_string(cr.status) +
                 (detail.empty() ? std::string("") : " " + detail));
        }
        return 0;
    }

    // Резолвит динамические рубрики статьи (иерархические пути "РуA > РуB")
    // в список id, соблюдая родительско-дочернюю структуру.
    std::vector<int> resolve_categories(const Article& a, const NetworkConfig& nc,
                                        const std::string& auth) {
        std::vector<int> ids;
        for (const std::string& path : a.categories_ru) {
            std::vector<std::string> levels = split_taxonomy_path(path);
            int parent = 0;
            for (const std::string& lvl : levels) {
                const int id = resolve_term("categories", lvl, parent, nc, auth);
                if (id == 0) break;  // уровень не создался — глубже нет смысла
                ids.push_back(id);
                parent = id;
            }
        }
        return ids;
    }

    // Резолвит динамические теги статьи (плоский список) в список id.
    std::vector<int> resolve_tags(const Article& a, const NetworkConfig& nc,
                                  const std::string& auth) {
        std::vector<int> ids;
        for (const std::string& t : a.tags_ru) {
            const int id = resolve_term("tags", t, 0, nc, auth);
            if (id != 0) ids.push_back(id);
        }
        return ids;
    }

    // Ищет на выходном сайте похожие материалы по тегам статьи (либо по
    // ключевому слову, если тегов нет) и возвращает до max записей. Используется
    // для внутренней перелинковки (links.internal_related). Если подходящих нет
    // — возвращает пустой список (в блок «Читайте также» ничего не добавляем).
    std::vector<RelatedPost> find_related_posts(const NetworkConfig& nc,
                                                const std::string& auth,
                                                const std::vector<int>& tag_ids,
                                                int max,
                                                const std::string& search_term) {
        if (max <= 0) return {};
        std::string ep = site_url_ + "/wp-json/wp/v2/" + post_type_ +
                         "?per_page=" + std::to_string(max) +
                         "&status=publish&_fields=title,link";
        if (!tag_ids.empty()) {
            ep += "&tags[]=";
            for (std::size_t i = 0; i < tag_ids.size(); ++i) {
                if (i) ep += "&tags[]=";
                ep += std::to_string(tag_ids[i]);
            }
        } else if (!search_term.empty()) {
            ep += "&search=" + url_encode(search_term);
        } else {
            return {};   // не по чем искать — не добавляем блок
        }
        HttpResponse r = client_.get(
            ep, nc, std::vector<std::string>{"Authorization: Basic " + auth});
        if (!r.ok || r.status != 200) {
            if (log_) {
                log_("WordPressSink: не удалось найти похожие материалы "
                     "(HTTP " + std::to_string(r.status) + ")");
            }
            return {};
        }
        bool ok = false;
        Json arr = Json::parse(r.body, &ok);
        std::vector<RelatedPost> out;
        if (ok && arr.is_array()) {
            for (std::size_t i = 0; i < arr.size() && (int)out.size() < max; ++i) {
                const Json& item = arr[i];
                if (!item.is_object()) continue;
                RelatedPost rp;
                rp.title = strip_html(item.get("title").get("rendered").as_string());
                rp.link = item.get("link").as_string();
                if (rp.link.empty()) continue;
                out.push_back(std::move(rp));
            }
        }
        return out;
    }

    // Возвращает id дефолтной рубрики WP («Без рубрики»/uncategorized, parent=0).
    // Кешируется на время жизни sink. Нужно, чтобы не дублировать её с нашими
    // назначениями (иначе пост попадает и в подходящую рубрику, и в «Без
    // рубрики» одновременно).
    int default_category_id(const NetworkConfig& nc, const std::string& auth) {
        if (default_category_id_ != 0) return default_category_id_;
        const std::string ep = site_url_ +
            "/wp-json/wp/v2/categories?per_page=100&_fields=id,name,slug,parent";
        HttpResponse r = client_.get(
            ep, nc, std::vector<std::string>{"Authorization: Basic " + auth});
        if (r.ok && r.status == 200) {
            bool ok = false;
            Json arr = Json::parse(r.body, &ok);
            if (ok && arr.is_array()) {
                for (std::size_t i = 0; i < arr.size(); ++i) {
                    const Json& item = arr[i];
                    if (!item.is_object()) continue;
                    const int parent = static_cast<int>(item.get("parent").as_int(0));
                    if (parent != 0) continue;
                    const std::string slug = item.get("slug").as_string();
                    const std::string name = item.get("name").as_string();
                    if (slug == "uncategorized" || name == "Без рубрики" ||
                        name == "Uncategorized" || name == "Uncat") {
                        default_category_id_ =
                            static_cast<int>(item.get("id").as_int(0));
                        break;
                    }
                }
            }
        }
        return default_category_id_;
    }

    HttpClient client_;
    std::string env_path_;      // <data_dir>/news_rewriter/.env (секреты)
    Storage& storage_;         // для стабильного slug-а (сверка с сайтом)
    bool verify_site_state_ = true;  // сверять дедуп с реальным состоянием сайта
    std::string site_url_;
    std::string username_;
    std::string app_password_;
    std::string status_;
    std::string post_type_;
    std::string excerpt_;
    std::string slug_;
    std::string featured_image_;
    std::vector<int> categories_;
    std::vector<int> tags_;
    int author_ = 0;
    bool taxonomy_auto_assign_ = true;  // проставлять динамическую таксономию в WP
    int internal_related_max_ = 2;      // число внутренних ссылок (1..2)
    std::string external_links_mode_ = "source";  // source | all | none
    int default_category_id_ = 0;       // кэш id рубрики «Без рубрики» (uncategorized)
    int timeout_ = 20;
    int max_retries_ = 0;
    int retry_delay_ms_ = 1000;
    LogFn log_;
};

// Список доступных типов записей (включая пользовательские) для подсказки
// пользователю — возвращает «Название (slug)» через запятую.
std::string wordpress_list_types(HttpClient& client, const std::string& site_url) {
    const std::string ep = site_url + "/wp-json/wp/v2/types";
    NetworkConfig nc;
    nc.timeout_seconds = 10;
    HttpResponse r = client.get(ep, nc);
    if (!r.ok || r.status != 200) return "(не удалось получить список типов)";
    bool ok = false;
    Json j = Json::parse(r.body, &ok);
    if (!ok || !j.is_object()) return "(нет данных о типах)";
    std::string out;
    for (const std::string& key : j.keys()) {
        const Json& v = j.get(key);
        const std::string rb = v.get("rest_base").as_string();
        const std::string nm = v.get("name").as_string();
        if (!out.empty()) out += ", ";
        if (nm.empty()) out += key;
        else out += nm + " (" + (rb.empty() ? key : rb) + ")";
    }
    return out.empty() ? "(типов нет)" : out;
}

} // namespace

// Регистрируется в ll_plugin_init под "wordpress" (см. plugin_main.cpp).
std::unique_ptr<Sink> make_wordpress_sink(const SinkConfig& cfg, Storage& storage,
                                          const LogFn& log) {
    return std::make_unique<WordPressSink>(cfg, storage, log);
}

std::string wordpress_check_connection(const std::string& site_url,
                                       const std::string& user,
                                       const std::string& app_password) {
    const std::string su = rtrim(site_url, '/');
    std::string pass = app_password;
    {
        std::string norm;
        for (char c : pass) if (c != ' ') norm += c;
        pass = norm;
    }
    if (su.empty() || user.empty() || pass.empty())
        return "не заданы site_url / логин / пароль";

    HttpClient client;
    if (!client.init()) return "libcurl недоступен";

    const std::string endpoint = su + "/wp-json/wp/v2/users/me";
    const std::string auth = "Authorization: Basic " + base64_encode(user + ":" + pass);
    NetworkConfig nc;
    nc.timeout_seconds = 15;
    HttpResponse resp = client.get(endpoint, nc,
                                   std::vector<std::string>{auth});
    if (!resp.ok)
        return "сайт недоступен: " + (resp.error.empty() ? "нет ответа" : resp.error);
    if (resp.status == 200) {
        bool ok = false;
        Json j = Json::parse(resp.body, &ok);
        std::string name = ok ? j.get("name").as_string() : std::string();
        std::string result = "OK: авторизован" +
                             (name.empty() ? std::string("") : " как «" + name + "»");
        result += "\nТипы записей WP:\n" + wordpress_list_types(client, su);
        return result;
    }
    if (resp.status == 401 || resp.status == 403) {
        std::string body = resp.body;
        if (body.size() > 300) body = body.substr(0, 300) + "…";
        return "ошибка авторизации (HTTP " + std::to_string(resp.status) +
               "): " + (body.empty() ? "нет тела ответа (возможно, сервер "
                                       "не передаёт заголовок Authorization)"
                                     : body);
    }
    return "HTTP " + std::to_string(resp.status) +
           (resp.body.empty() ? std::string("") : ": " + resp.body);
}

} // namespace news_rewriter
