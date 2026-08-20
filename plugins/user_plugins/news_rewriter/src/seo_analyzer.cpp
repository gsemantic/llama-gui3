#include "seo_analyzer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <unordered_set>

namespace news_rewriter {

namespace {

// ---- UTF-8 helpers ---------------------------------------------------------

std::vector<uint32_t> utf8_to_cps(const std::string& s) {
    std::vector<uint32_t> out;
    std::size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0;
        if (c < 0x80) {
            cp = c; ++i;
        } else if ((c & 0xE0) == 0xC0) {
            cp = (c & 0x1F);
            if (i + 1 < s.size()) cp = (cp << 6) | (s[i + 1] & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = (c & 0x0F);
            if (i + 1 < s.size()) cp = (cp << 6) | (s[i + 1] & 0x3F);
            if (i + 2 < s.size()) cp = (cp << 6) | (s[i + 2] & 0x3F);
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = (c & 0x07);
            if (i + 1 < s.size()) cp = (cp << 6) | (s[i + 1] & 0x3F);
            if (i + 2 < s.size()) cp = (cp << 6) | (s[i + 2] & 0x3F);
            if (i + 3 < s.size()) cp = (cp << 6) | (s[i + 3] & 0x3F);
            i += 4;
        } else {
            ++i;
            continue;
        }
        out.push_back(cp);
    }
    return out;
}

// Декодирует один Unicode-codepoint, начинающийся в s[j] (UTF-8).
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

std::string cp_to_utf8(uint32_t cp) {
    std::string s;
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return s;
}

std::string to_lower_utf8(const std::string& s) {
    std::string out;
    for (uint32_t cp : utf8_to_cps(s)) {
        if (cp >= 'A' && cp <= 'Z') {
            cp = cp - 'A' + 'a';
        } else if (cp >= 0x410 && cp <= 0x42F) {  // кириллица заглавная
            cp += 0x20;
        }
        out += cp_to_utf8(cp);
    }
    return out;
}

bool is_word_char(unsigned char c) {
    if (std::isalpha(c)) return true;
    if (c >= 0x80) return true;          // любой байт многобайтового символа
                                        // (ведущий 0xC0–0xFF и продолжение 0x80–0xBF)
    if (std::isdigit(c)) return true;
    if (c == '-') return true;           // дефис внутри слова (по-русски)
    return false;
}

std::string trim(const std::string& s) {
    std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    std::size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool ci_contains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    return to_lower_utf8(haystack).find(to_lower_utf8(needle)) != std::string::npos;
}

bool ends_with(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

// ---- Сокращения, после которых точка НЕ заканчивает предложение ----------
bool is_abbreviation(const std::string& tok) {
    static const char* kAbbr[] = {
        "т.д", "т.п", "др", "г", "стр", "рис", "с", "им", "проф", "д-р", "т.е",
        "напр", "и.о", "см", "п", "гл", "яз", "ред", "табл", "ч", "зам", "акад",
        "e.g", "i.e", "etc", "vs", "mr", "mrs", "dr", "no", "vol", "fig", "sth",
        "cdn", "inc", "ltd", "co", "corp", "dept", "est", "approx"
    };
    std::string t = to_lower_utf8(trim(tok));
    for (const char* a : kAbbr) {
        if (t == a) return true;
    }
    return false;
}

// ---- Словари переходных слов ---------------------------------------------
const std::unordered_set<std::string>& ru_transitions() {
    static const std::string k[] = {
        "кроме", "при", "этом", "однако", "более", "например", "следовательно",
        "частности", "несмотря", "поэтому", "таким", "образом", "прежде",
        "всего", "итак", "значит", "также", "впрочем", "наконец", "вопреки",
        "благодаря", "согласно", "напротив", "вместе", "будущем", "случае",
        "действительности", "вдобавок", "сверх", "притом", "причем", "впоследствии",
        "сначала", "затем", "потом", "впрочем", "наоборот", "вобщем", "следом"
    };
    static std::unordered_set<std::string> s(std::begin(k), std::end(k));
    return s;
}

const std::unordered_set<std::string>& en_transitions() {
    static const std::string k[] = {
        "however", "moreover", "therefore", "example", "addition", "consequently",
        "furthermore", "thus", "other", "hand", "first", "finally", "because",
        "besides", "meanwhile", "nevertheless", "nonetheless", "similarly",
        "likewise", "instead", "otherwise", "specifically", "namely", "indeed",
        "certainly", "perhaps", "then", "next", "still", "yet", "although",
        "though", "since", "unless", "whereas", "while", "also", "additionally",
        "further", "accordingly", "hence", "conversely", "notably", "overall"
    };
    static std::unordered_set<std::string> s(std::begin(k), std::end(k));
    return s;
}

bool is_ru_vowel(uint32_t cp) {
    switch (cp) {
        case 0x430: case 0x435: case 0x451: case 0x438: case 0x43E:
        case 0x443: case 0x44B: case 0x44D: case 0x44E: case 0x44F:
            return true;
        default:
            return false;
    }
}

bool is_en_vowel(uint32_t cp) {
    cp |= 0x20;  // to lower
    return cp == 'a' || cp == 'e' || cp == 'i' || cp == 'o' || cp == 'u' || cp == 'y';
}

// Формы связки "быть" (RU) и be-глаголы (EN) для детекта пассива.
bool is_ru_be_form(const std::string& w) {
    static const char* k[] = {
        "был", "была", "было", "были", "буду", "будешь", "будет", "будем",
        "будете", "будут", "будучи", "есть", "суть", "является", "являются",
        "явился", "явилась", "явилось", "явились", "стал", "стала", "стало",
        "стали", "оказался", "оказалась", "оказалось", "оказались"
    };
    for (const char* s : k) if (w == s) return true;
    return false;
}

bool is_en_be_form(const std::string& w) {
    static const char* k[] = {
        "am", "is", "are", "was", "were", "be", "been", "being",
        "get", "got", "gotten", "become", "became"
    };
    for (const char* s : k) if (w == s) return true;
    return false;
}

bool is_ru_passive_participle(const std::string& w) {
    if (w.size() < 4) return false;
    return ends_with(w, "ан") || ends_with(w, "ана") || ends_with(w, "ано") ||
           ends_with(w, "аны") || ends_with(w, "ен") || ends_with(w, "ена") ||
           ends_with(w, "ено") || ends_with(w, "ены") || ends_with(w, "ён") ||
           ends_with(w, "ёна") || ends_with(w, "ёно") || ends_with(w, "ёны") ||
           ends_with(w, "нут") || ends_with(w, "нута") || ends_with(w, "нуто") ||
           ends_with(w, "нуты");
}

std::string status_word(SeoStatus s) {
    return s == SeoStatus::Good ? "GOOD" : (s == SeoStatus::Ok ? "OK" : "POOR");
}

} // namespace

// ============================================================================
// Публичные методы
// ============================================================================

std::vector<std::string> SeoAnalyzer::split_paragraphs(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n' && i + 1 < text.size() && text[i + 1] == '\n') {
            std::string p = trim(cur);
            if (!p.empty()) out.push_back(p);
            cur.clear();
            ++i;
            continue;
        }
        cur += text[i];
    }
    std::string p = trim(cur);
    if (!p.empty()) out.push_back(p);
    return out;
}

std::vector<std::string> SeoAnalyzer::split_sentences(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    const int n = static_cast<int>(text.size());
    for (int i = 0; i < n; ++i) {
        char c = text[i];
        cur += c;
        if (c == '.' || c == '!' || c == '?') {
            if (i > 0 && text[i - 1] == '.') continue;  // многоточие
            if (c == '.') {
                // десятичная дробь: цифра до И после точки (напр. "3.14")
                bool dec = (i > 0 && std::isdigit(static_cast<unsigned char>(text[i - 1]))) &&
                           (i + 1 < n && std::isdigit(static_cast<unsigned char>(text[i + 1])));
                if (dec) continue;
                // сокращение: токен прямо перед точкой
                int s = static_cast<int>(cur.size()) - 1;  // индекс точки в cur
                int k = s - 1;
                while (k >= 0) {
                    char d = cur[k];
                    if (d == ' ' || d == '\n' || d == '\t' || d == '\r') break;
                    --k;
                }
                if (s - 1 >= k + 1) {
                    std::string tok = cur.substr(k + 1, s - 1 - k);
                    if (is_abbreviation(tok)) continue;
                }
            }
            int j = i + 1;
            while (j < n && (text[j] == ' ' || text[j] == '\n' || text[j] == '\t' || text[j] == '\r')) ++j;
            if (j >= n) {
                out.push_back(trim(cur));
                cur.clear();
            } else {
                uint32_t nx = cp_at(text, static_cast<std::size_t>(j));
                bool upper = (nx >= 'A' && nx <= 'Z') || (nx >= 0x410 && nx <= 0x42F);
                if (upper || (nx >= '0' && nx <= '9') ||
                    nx == '"' || nx == '\'' || nx == '(') {
                    out.push_back(trim(cur));
                    cur.clear();
                }
            }
        }
    }
    std::string last = trim(cur);
    if (!last.empty()) out.push_back(last);
    return out;
}

std::vector<std::string> SeoAnalyzer::split_words(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (unsigned char c : text) {
        if (is_word_char(c)) {
            cur += static_cast<char>(c);
        } else {
            if (!cur.empty()) out.push_back(to_lower_utf8(cur));
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(to_lower_utf8(cur));
    return out;
}

int SeoAnalyzer::count_syllables(const std::string& word, const std::string& lang) {
    std::string w = to_lower_utf8(word);
    std::vector<uint32_t> cps = utf8_to_cps(w);
    bool ru = lang.empty() ? false : (lang[0] == 'r' || lang[0] == 'R');
    int n = 0;
    if (ru) {
        for (uint32_t cp : cps) if (is_ru_vowel(cp)) ++n;
    } else {
        bool prev = false;
        for (uint32_t cp : cps) {
            bool v = is_en_vowel(cp);
            if (v && !prev) ++n;
            prev = v;
        }
    }
    return n < 1 ? 1 : n;
}

bool SeoAnalyzer::has_transition_word(const std::string& sentence,
                                      const std::string& lang) {
    bool ru = lang.empty() ? true : (lang[0] == 'r' || lang[0] == 'R');
    const auto& set = ru ? ru_transitions() : en_transitions();
    for (const std::string& w : split_words(sentence)) {
        if (set.count(w)) return true;
    }
    return false;
}

bool SeoAnalyzer::is_passive(const std::string& sentence, const std::string& lang) {
    std::vector<std::string> words = split_words(sentence);
    if (words.empty()) return false;
    bool ru = lang.empty() ? true : (lang[0] == 'r' || lang[0] == 'R');
    if (ru) {
        bool be = false, part = false;
        for (const std::string& w : words) {
            if (is_ru_be_form(w)) be = true;
            if (is_ru_passive_participle(w)) part = true;
        }
        return part && be;
    } else {
        for (std::size_t i = 0; i < words.size(); ++i) {
            if (is_en_be_form(words[i])) {
                if (i + 1 < words.size() &&
                    (ends_with(words[i + 1], "ed") || ends_with(words[i + 1], "en"))) {
                    return true;
                }
            }
        }
        return false;
    }
}

double SeoAnalyzer::flesch(const std::string& text, const std::string& lang,
                            const SeoCriteria& crit) {
    std::vector<std::string> sentences = split_sentences(text);
    int ns = 0;
    for (const auto& s : sentences) if (!s.empty()) ++ns;
    std::vector<std::string> words = split_words(text);
    int nw = static_cast<int>(words.size());
    if (ns == 0 || nw == 0) return 0.0;
    double asl = static_cast<double>(nw) / ns;

    bool ru = !lang.empty() && (lang[0] == 'r' || lang[0] == 'R');
    if (ru) {
        // ASL-индекс: короткие предложения = легче. Формула 130 - 4.5*ASL,
        // обрезанная в [0,100]. ASL=8 -> ~94, ASL=15 -> ~63, ASL>=29 -> 0.
        double v = 130.0 - 4.5 * asl;
        if (v < 0.0) v = 0.0;
        if (v > 100.0) v = 100.0;
        return v;
    }

    int syll = 0;
    for (const auto& w : words) syll += count_syllables(w, lang);
    double asw = static_cast<double>(syll) / nw;
    double a = crit.flesch_a, b = crit.flesch_b, c = crit.flesch_c;
    return a - b * asl - c * asw;
}

std::vector<std::string> SeoAnalyzer::extract_headings(const std::string& body) {
    std::vector<std::string> out;
    std::stringstream ss(body);
    std::string line;
    while (std::getline(ss, line, '\n')) {
        std::string t = trim(line);
        if (t.rfind("# ", 0) == 0 && t.size() > 2) {
            out.push_back(t.substr(2));
        }
    }
    return out;
}

// ============================================================================
// Полный анализ
// ============================================================================

SeoReport SeoAnalyzer::analyze(const std::string& body,
                               const std::string& title,
                               const std::string& focus_keyword,
                               const std::string& lang,
                               const SeoCriteria& crit) {
    SeoReport r;
    bool ru = !lang.empty() && (lang[0] == 'r' || lang[0] == 'R');
    auto status_for = [](double val, double good_max, double poor_min) -> SeoStatus {
        if (val <= good_max) return SeoStatus::Good;
        if (val < poor_min) return SeoStatus::Ok;
        return SeoStatus::Poor;
    };

    std::vector<std::string> paragraphs = split_paragraphs(body);
    std::vector<std::string> sentences = split_sentences(body);
    std::vector<std::string> words = split_words(body);
    int total_words = static_cast<int>(words.size());
    int nsent = 0;
    for (const auto& s : sentences) if (!s.empty()) ++nsent;

    int max_sent_words = 0, sum_sent_words = 0;
    int transition_sent = 0, passive_sent = 0;
    for (const auto& s : sentences) {
        if (s.empty()) continue;
        int c = static_cast<int>(split_words(s).size());
        max_sent_words = std::max(max_sent_words, c);
        sum_sent_words += c;
        if (has_transition_word(s, lang)) ++transition_sent;
        if (is_passive(s, lang)) ++passive_sent;
    }
    double avg_sent = nsent > 0 ? static_cast<double>(sum_sent_words) / nsent : 0.0;
    double trans_ratio = nsent > 0 ? static_cast<double>(transition_sent) / nsent : 0.0;
    double passive_ratio = nsent > 0 ? static_cast<double>(passive_sent) / nsent : 0.0;

    int max_par_words = 0;
    for (const auto& p : paragraphs) {
        max_par_words = std::max(max_par_words, static_cast<int>(split_words(p).size()));
    }

    double flesch_val = flesch(body, lang, crit);

    // Подзаголовки и слова до первого из них.
    std::vector<std::string> headings = extract_headings(body);
    int words_before_heading = 0;
    bool seen_heading = false;
    for (const auto& p : paragraphs) {
        if (!seen_heading && p.rfind("# ", 0) == 0) {
            seen_heading = true;
            continue;
        }
        if (!seen_heading) {
            words_before_heading += static_cast<int>(split_words(p).size());
        }
    }

    // Ключевая фраза.
    std::string kp = to_lower_utf8(trim(focus_keyword));
    bool in_title = kp.empty() ? true : ci_contains(title, kp);
    bool in_first_par = kp.empty() ? true :
        (!paragraphs.empty() && ci_contains(paragraphs.front(), kp));
    bool in_heading = kp.empty() ? true :
        std::any_of(headings.begin(), headings.end(),
                    [&](const std::string& h) { return ci_contains(h, kp); });
    double density = 0.0;
    if (!kp.empty() && total_words > 0) {
        std::string low = to_lower_utf8(body);
        std::string needle = kp;
        std::size_t pos = 0, occ = 0;
        while ((pos = low.find(needle, pos)) != std::string::npos) {
            ++occ;
            pos += needle.size();
        }
        density = static_cast<double>(occ) / total_words;
    }

    // Последовательные одинаковые начальные слова.
    int max_same_start = 0;
    for (const auto& p : paragraphs) {
        int run = 1;
        std::string prev;
        for (const auto& s : split_sentences(p)) {
            std::vector<std::string> w = split_words(s);
            if (w.empty()) continue;
            if (!prev.empty() && w.front() == prev) {
                ++run;
                max_same_start = std::max(max_same_start, run);
            } else {
                run = 1;
            }
            prev = w.front();
        }
    }

    // ---- Формируем метрики ----
    auto add = [&](const std::string& key, const std::string& label,
                   double val, const std::string& text, SeoStatus st) {
        r.metrics.push_back({key, label, val, text, st});
    };

    add("words_total", "Объём статьи (слов)", total_words,
        std::to_string(total_words) + " сл.",
        total_words >= crit.min_words ? SeoStatus::Good :
        (total_words >= crit.min_words / 2 ? SeoStatus::Ok : SeoStatus::Poor));

    add("sentence_max", "Макс. длина предложения", max_sent_words,
        std::to_string(max_sent_words) + " сл.",
        status_for(max_sent_words, crit.max_sentence_words,
                   crit.max_sentence_words * 1.4 + 1));

    add("sentence_avg", "Средняя длина предложения", avg_sent,
        std::to_string(static_cast<int>(avg_sent + 0.5)) + " сл.",
        status_for(avg_sent, crit.max_sentence_words, crit.max_sentence_words * 1.4 + 1));

    add("paragraph_max", "Макс. длина абзаца", max_par_words,
        std::to_string(max_par_words) + " сл.",
        status_for(max_par_words, crit.max_paragraph_words,
                   crit.max_paragraph_words * 1.4 + 1));

    add("transition_ratio", "Доля предл. с переходными словами", trans_ratio,
        std::to_string(static_cast<int>(trans_ratio * 100 + 0.5)) + "%",
        trans_ratio >= crit.min_transition_ratio ? SeoStatus::Good :
        (trans_ratio >= crit.min_transition_ratio * 0.7 ? SeoStatus::Ok : SeoStatus::Poor));

    add("passive_ratio", "Доля пассивных предложений", passive_ratio,
        std::to_string(static_cast<int>(passive_ratio * 100 + 0.5)) + "%",
        passive_ratio <= crit.max_passive_ratio * 0.5 ? SeoStatus::Good :
        (passive_ratio <= crit.max_passive_ratio ? SeoStatus::Ok : SeoStatus::Poor));

    add("flesch", "Удобочитаемость (читаемость)", flesch_val,
        std::to_string(static_cast<int>(flesch_val + 0.5)),
        ru ? ((flesch_val >= crit.ru_read_ease_good) ? SeoStatus::Good :
              (flesch_val >= crit.ru_read_ease_ok ? SeoStatus::Ok : SeoStatus::Poor))
           : ((flesch_val >= crit.flesch_min && flesch_val <= crit.flesch_max) ? SeoStatus::Good :
              (flesch_val >= crit.flesch_min - 15 && flesch_val <= crit.flesch_max + 15 ?
                   SeoStatus::Ok : SeoStatus::Poor)));

    add("keyphrase_title", "Ключ. фраза в заголовке", in_title ? 1 : 0,
        in_title ? "да" : "нет",
        (kp.empty() || in_title) ? SeoStatus::Good : SeoStatus::Poor);

    add("keyphrase_first_paragraph", "Ключ. фраза в 1-м абзаце", in_first_par ? 1 : 0,
        in_first_par ? "да" : "нет",
        (kp.empty() || in_first_par) ? SeoStatus::Good : SeoStatus::Poor);

    add("keyphrase_heading", "Ключ. фраза в подзаголовке", in_heading ? 1 : 0,
        in_heading ? "да" : "нет",
        (kp.empty() || !crit.require_keyphrase_one_heading || in_heading) ?
            SeoStatus::Good : SeoStatus::Poor);

    add("keyphrase_density", "Плотность ключ. фразы", density,
        std::to_string(static_cast<int>(density * 1000 + 0.5) / 10.0) + "%",
        (density >= crit.keyphrase_density_min && density <= crit.keyphrase_density_max) ?
            SeoStatus::Good :
        (density >= crit.keyphrase_density_min * 0.5 && density <= crit.keyphrase_density_max * 1.5) ?
            SeoStatus::Ok : SeoStatus::Poor);

    add("words_before_heading", "Слов до 1-го подзаголовка", words_before_heading,
        std::to_string(words_before_heading) + " сл.",
        (headings.empty() || words_before_heading <= crit.max_words_before_first_heading) ?
            SeoStatus::Good : SeoStatus::Poor);

    add("consecutive_same_start", "Повтор стартового слова (подряд)", max_same_start,
        std::to_string(max_same_start),
        max_same_start <= crit.max_consecutive_same_start ? SeoStatus::Good : SeoStatus::Poor);

    // ---- Итог ----
    double sum = 0;
    for (const auto& m : r.metrics) {
        sum += (m.status == SeoStatus::Good ? 1.0 :
                (m.status == SeoStatus::Ok ? 0.6 : 0.0));
        if (m.status == SeoStatus::Poor) r.issues.push_back(m.label);
    }
    r.score = static_cast<int>(100.0 * sum / r.metrics.size() + 0.5);
    return r;
}

std::string SeoReport::summary() const {
    std::ostringstream oss;
    oss << "SEO " << score << "/100";
    if (!issues.empty()) {
        oss << " (POOR: ";
        for (std::size_t i = 0; i < issues.size(); ++i) {
            if (i) oss << ", ";
            oss << issues[i];
        }
        oss << ")";
    }
    return oss.str();
}

} // namespace news_rewriter
