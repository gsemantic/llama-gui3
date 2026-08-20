#include "translit.h"

#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>

namespace news_rewriter {

namespace {

// Побайтовый декод одного Unicode code point (UTF-8). Возвращает false в конце.
bool next_cp(const std::string& s, std::size_t& i, unsigned int& cp) {
    if (i >= s.size()) return false;
    const unsigned char b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) { cp = b0; i += 1; return true; }
    int n = 0;
    if ((b0 & 0xE0) == 0xC0) { n = 1; cp = b0 & 0x1F; }
    else if ((b0 & 0xF0) == 0xE0) { n = 2; cp = b0 & 0x0F; }
    else if ((b0 & 0xF8) == 0xF0) { n = 3; cp = b0 & 0x07; }
    else { cp = b0; i += 1; return true; }
    const std::size_t end = i + 1 + n;
    if (end > s.size()) { cp = b0; i += 1; return true; }
    for (int k = 1; k <= n; ++k) {
        const unsigned char bk = static_cast<unsigned char>(s[i + k]);
        if ((bk & 0xC0) != 0x80) { cp = b0; i += 1; return true; }
        cp = (cp << 6) | (bk & 0x3F);
    }
    i = end;
    return true;
}

// Транслитерация одной кириллической строчной буквы в латиницу (nullptr — не RU).
const char* ru_lower_to_latin(unsigned int cp) {
    switch (cp) {
        case 0x430: return "a";    // а
        case 0x431: return "b";    // б
        case 0x432: return "v";    // в
        case 0x433: return "g";    // г
        case 0x434: return "d";    // д
        case 0x435: return "e";    // е
        case 0x451: return "e";    // ё
        case 0x436: return "zh";   // ж
        case 0x437: return "z";    // з
        case 0x438: return "i";    // и
        case 0x439: return "i";    // й
        case 0x43A: return "k";    // к
        case 0x43B: return "l";    // л
        case 0x43C: return "m";    // м
        case 0x43D: return "n";    // н
        case 0x43E: return "o";    // о
        case 0x43F: return "p";    // п
        case 0x440: return "r";    // р
        case 0x441: return "s";    // с
        case 0x442: return "t";    // т
        case 0x443: return "u";    // у
        case 0x444: return "f";    // ф
        case 0x445: return "h";    // х
        case 0x446: return "c";    // ц
        case 0x447: return "ch";   // ч
        case 0x448: return "sh";   // ш
        case 0x449: return "sch";  // щ
        case 0x44A: return "";     // ъ (твёрдый знак — опускаем)
        case 0x44B: return "y";    // ы
        case 0x44C: return "";     // ь (мягкий знак — опускаем)
        case 0x44D: return "e";    // э
        case 0x44E: return "yu";   // ю
        case 0x44F: return "ya";   // я
        default: return nullptr;
    }
}

bool is_stopword(const std::string& w) {
    static const char* kStop[] = {
        "i", "v", "vo", "na", "s", "so", "k", "o", "ob", "ot", "do", "po", "za",
        "iz", "u", "a", "the", "a", "an", "of", "to", "in", "on", "for", "and",
        "or", "is", "are", "was", "were", "this", "that", "with", "as", "at",
        "by", "from", "it"
    };
    for (const char* s : kStop) if (w == s) return true;
    return false;
}

} // namespace

std::string transliterate_to_latin(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    unsigned int cp = 0;
    while (next_cp(s, i, cp)) {
        if (cp >= 0x410 && cp <= 0x42F) {        // заглавная кириллица (А–Я)
            const unsigned int lc = (cp == 0x401) ? 0x451 : cp + 0x20;
            const char* t = ru_lower_to_latin(lc);
            if (t) { for (const char* p = t; *p; ++p) out += static_cast<char>(std::toupper(*p)); }
        } else if (cp == 0x451 || (cp >= 0x430 && cp <= 0x44F)) {  // строчная (а–я, ё)
            const char* t = ru_lower_to_latin(cp);
            if (t) out += t;
        } else if (cp < 0x80) {
            out += static_cast<char>(cp);
        }
        // Прочие многобайтовые не-кириллические символы — не добавляем.
    }
    return out;
}

std::string make_slug(const std::string& s) {
    // 1) Транслитерация + нижний регистр.
    std::string t = transliterate_to_latin(s);
    std::string low;
    low.reserve(t.size());
    for (char c : t) low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // 2) Токенизация по не-буквам/цифрам; фильтрация стоп-слов.
    std::string token;
    std::vector<std::string> words;
    auto flush = [&]() {
        if (token.empty()) return;
        if (!is_stopword(token)) words.push_back(token);
        token.clear();
    };
    for (char c : low) {
        const bool alnum = std::isalnum(static_cast<unsigned char>(c)) != 0;
        if (alnum) token += c;
        else flush();
    }
    flush();

    // 3) Склейка через '-'.
    std::string slug;
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (i) slug += '-';
        slug += words[i];
    }
    return slug;
}

} // namespace news_rewriter
