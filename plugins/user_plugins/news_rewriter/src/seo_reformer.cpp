#include "seo_reformer.h"

#include <algorithm>
#include <cctype>

namespace news_rewriter {

namespace {

std::string trim(const std::string& s) {
    std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    std::size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool is_word_byte(unsigned char c) {
    return std::isalpha(c) || c >= 0x80 || std::isdigit(c) || c == '-';
}

// Границы слов в строке (буквы/цифры/дефис), без пунктуации.
std::vector<std::pair<std::size_t, std::size_t>> word_spans(const std::string& s) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    std::size_t i = 0, n = s.size();
    while (i < n) {
        if (is_word_byte(static_cast<unsigned char>(s[i]))) {
            std::size_t start = i;
            while (i < n && is_word_byte(static_cast<unsigned char>(s[i]))) ++i;
            out.push_back({start, i});
        } else {
            ++i;
        }
    }
    return out;
}

uint32_t cp_at(const std::string& s, std::size_t j) {
    if (j >= s.size()) return 0;
    unsigned char c = static_cast<unsigned char>(s[j]);
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && j + 1 < s.size())
        return ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[j + 1]) & 0x3F);
    if ((c & 0xF0) == 0xE0 && j + 2 < s.size())
        return ((c & 0x0F) << 12) | ((static_cast<unsigned char>(s[j + 1]) & 0x3F) << 6) |
               (static_cast<unsigned char>(s[j + 2]) & 0x3F);
    if ((c & 0xF8) == 0xF0 && j + 3 < s.size())
        return ((c & 0x07) << 18) | ((static_cast<unsigned char>(s[j + 1]) & 0x3F) << 12) |
               ((static_cast<unsigned char>(s[j + 2]) & 0x3F) << 6) |
               (static_cast<unsigned char>(s[j + 3]) & 0x3F);
    return c;
}

// Кодирует один Unicode-codepoint в UTF-8 (для перезаписи символа).
std::string cp_to_utf8(uint32_t cp) {
    std::string s;
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return s;
}

// Капитализирует первый буквенный символ строки (RU/EN).
std::string capitalize_first(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i >= s.size()) return s;
    uint32_t cp = cp_at(s, i);
    uint32_t up = 0;
    if (cp >= 'a' && cp <= 'z') {
        up = cp - 'a' + 'A';
    } else if (cp >= 0x430 && cp <= 0x44F) {  // кириллица строчная -> заглавная
        up = cp - 0x20;
    } else {
        return s;
    }
    // длина символа в байтах
    int len = (cp < 0x80) ? 1 : (cp < 0x800 ? 2 : 3);
    std::string out = s;
    std::string enc = cp_to_utf8(up);
    out.replace(i, len, enc);
    return out;
}

bool is_heading(const std::string& para) {
    std::string t = trim(para);
    return t.rfind("#", 0) == 0;
}

std::string join_paragraphs(const std::vector<std::string>& ps) {
    std::string r;
    for (std::size_t i = 0; i < ps.size(); ++i) {
        if (i) r += "\n\n";
        r += ps[i];
    }
    return r;
}

// Склеивает предложения в один абзац (через пробел).
std::string glue_sentences(const std::vector<std::string>& ss) {
    std::string r;
    for (const auto& s : ss) {
        if (!r.empty()) r += " ";
        r += s;
    }
    return r;
}

} // namespace

// ============================================================================
std::string SeoReformer::split_long_paragraphs(const std::string& body, int max_words) {
    if (max_words <= 0) return body;
    std::vector<std::string> paras = SeoAnalyzer::split_paragraphs(body);
    std::vector<std::string> out;
    for (const auto& p : paras) {
        if (is_heading(p)) { out.push_back(p); continue; }
        if (static_cast<int>(SeoAnalyzer::split_words(p).size()) <= max_words) {
            out.push_back(p);
            continue;
        }
        std::vector<std::string> sents = SeoAnalyzer::split_sentences(p);
        if (sents.size() <= 1) {
            // Один гигантский абзац из одного предложения — дробить негде,
            // оставляем (пометится как poor в скоркарде).
            out.push_back(p);
            continue;
        }
        std::string cur;
        int cur_w = 0;
        for (const auto& s : sents) {
            int sw = static_cast<int>(SeoAnalyzer::split_words(s).size());
            if (!cur.empty() && cur_w + sw > max_words) {
                out.push_back(trim(cur));
                cur.clear();
                cur_w = 0;
            }
            if (!cur.empty()) cur += " ";
            cur += s;
            cur_w += sw;
        }
        if (!cur.empty()) out.push_back(trim(cur));
    }
    return join_paragraphs(out);
}

std::string SeoReformer::split_long_sentence(const std::string& sentence, int max_words,
                                             const std::string& /*lang*/) {
    if (max_words <= 0) return sentence;
    std::vector<std::pair<std::size_t, std::size_t>> spans = word_spans(sentence);
    int total = static_cast<int>(spans.size());
    if (total <= max_words) return sentence;

    // Лучшая точка разрыва — запятая, ближайшая по числу слов к середине.
    int target = total / 2;
    int best = -1, best_dist = 1 << 30;
    for (int i = 0; i < static_cast<int>(spans.size()); ++i) {
        std::size_t after = spans[i].second;
        if (after < sentence.size() && sentence[after] == ',') {
            int left_words = i + 1;
            int dist = std::abs(left_words - target);
            if (dist < best_dist) { best_dist = dist; best = i; }
        }
    }
    if (best < 0) {
        // Запятых нет — механически разбить нельзя, возвращаем как есть.
        return sentence;
    }

    std::size_t comma_pos = spans[best].second;       // индекс символа ','
    std::size_t split_at = comma_pos + 1;             // сразу после запятой
    while (split_at < sentence.size() && sentence[split_at] == ' ') ++split_at;

    std::string first = sentence.substr(0, comma_pos);  // без запятой
    first += '.';                                        // заменяем запятую на точку
    std::string second = capitalize_first(sentence.substr(split_at));

    return split_long_sentence(first, max_words) + " " +
           split_long_sentence(second, max_words);
}

std::string SeoReformer::add_transition(const std::string& sentence, const std::string& lang) {
    if (SeoAnalyzer::has_transition_word(sentence, lang)) return sentence;
    bool ru = lang.empty() ? true : (lang[0] == 'r' || lang[0] == 'R');
    std::string tw = ru ? "Кроме того, " : "Moreover, ";
    return tw + sentence;
}

SeoReformResult SeoReformer::reform(const std::string& body, const SeoReformConfig& cfg) {
    SeoReformResult res;
    std::string out = body;

    if (cfg.autofix_paragraphs) {
        auto before = SeoAnalyzer::split_paragraphs(out);
        out = split_long_paragraphs(out, cfg.max_paragraph_words);
        auto after = SeoAnalyzer::split_paragraphs(out);
        res.paragraphs_split = static_cast<int>(after.size()) - static_cast<int>(before.size());
    }

    if (cfg.autofix_sentences) {
        std::vector<std::string> paras = SeoAnalyzer::split_paragraphs(out);
        std::vector<std::string> newparas;
        int before_s = 0, after_s = 0;
        for (const auto& p : paras) {
            if (is_heading(p)) { newparas.push_back(p); continue; }
            auto bs = SeoAnalyzer::split_sentences(p);
            before_s += static_cast<int>(bs.size());
            std::vector<std::string> ns;
            for (const auto& s : bs)
                ns.push_back(split_long_sentence(s, cfg.max_sentence_words, cfg.lang));
            auto as = SeoAnalyzer::split_sentences(glue_sentences(ns));
            after_s += static_cast<int>(as.size());
            newparas.push_back(glue_sentences(ns));
        }
        out = join_paragraphs(newparas);
        res.sentences_split = after_s - before_s;
    }

    if (cfg.autofix_transitions) {
        std::vector<std::string> paras = SeoAnalyzer::split_paragraphs(out);
        std::vector<std::string> newparas;
        for (const auto& p : paras) {
            if (is_heading(p)) { newparas.push_back(p); continue; }
            std::vector<std::string> ns;
            for (const auto& s : SeoAnalyzer::split_sentences(p)) {
                std::string a = add_transition(s, cfg.lang);
                if (a != s) ++res.transitions_added;
                ns.push_back(a);
            }
            newparas.push_back(glue_sentences(ns));
        }
        out = join_paragraphs(newparas);
    }

    res.reformed_body = out;
    if (res.paragraphs_split == 0 && res.sentences_split == 0 && res.transitions_added == 0) {
        res.notes.push_back("Изменений не потребовалось: текст уже в пределах норм.");
    } else {
        if (res.paragraphs_split > 0)
            res.notes.push_back("Разбито абзацев: " + std::to_string(res.paragraphs_split) + ".");
        if (res.sentences_split > 0)
            res.notes.push_back("Разбито предложений: " + std::to_string(res.sentences_split) + ".");
        if (res.transitions_added > 0)
            res.notes.push_back("Добавлено переходных слов: " +
                                std::to_string(res.transitions_added) + ".");
    }
    return res;
}

} // namespace news_rewriter
