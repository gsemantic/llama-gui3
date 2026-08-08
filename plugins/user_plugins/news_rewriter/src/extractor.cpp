#include "extractor.h"

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

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
    return result;
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
    std::size_t noise_depth = 0;
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
                if (is_heading_tag(name)) heading_flag = true;
                // флаги фиксируются при создании блока, чтобы закрывающий
                // тег (например, </nav>) не «снял» их с уже идущего текста.
                cur.noise = (noise_depth > 0);
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

ExtractedArticle extract_page(const std::string& html, const SourceExtract& cfg) {
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
    return result;
}

} // namespace news_rewriter
