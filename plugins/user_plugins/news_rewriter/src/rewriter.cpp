#include "rewriter.h"

#include "translit.h"

#include <cctype>
#include <vector>

namespace news_rewriter {

// Обрезка текста до max байт с учётом многобайтовой (UTF-8) кодировки:
// не разрываем символ посередине (откатываемся за продолжения 0x80..0xBF).
std::string truncate_input(const std::string& s, int max) {
    if (max <= 0 || static_cast<int>(s.size()) <= max) return s;
    std::size_t cut = static_cast<std::size_t>(max);
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

namespace {

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

// Обрезка завершающих пробелов/переводов строк.
std::string trim_right(const std::string& s) {
    const std::size_t e = s.find_last_not_of(" \t\r\n");
    return e == std::string::npos ? std::string() : s.substr(0, e + 1);
}

// Схлопывание трёх и более подряд идущих '\n' в два (убираем «дыры» от
// вырезанных маркеров статьи в роли).
std::string collapse_blank_lines(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    int blank = 0;
    for (char c : s) {
        if (c == '\n') {
            ++blank;
            if (blank <= 2) out += c;
        } else {
            blank = 0;
            out += c;
        }
    }
    return out;
}

} // namespace

// Построение РОЛИ (системного промпта) из шаблона: заполняются статичные
// подстановки ({language}, {tone}, {max_words}), а маркеры статьи ({title},
// {body}) вместе с привычными подписями «Заголовок:»/«Текст:» ВЫРЕЗАЮТСЯ — они
// приходят в пользовательском сообщении (build_user_prompt). Так роль остаётся
// идентичной для всех новостей обхода и шлётся модели РОВНО ОДИН раз.
// with_max_words — добавлять ли инструкцию об объёме (для чистого рерайта;
// для SEO/комбинированного — false, т.к. объём задаётся иначе).
std::string build_role_from_template(const std::string& tpl,
                                     const RewriteConfig& cfg,
                                     bool with_max_words) {
    const bool had_max_words_ph = tpl.find("{max_words}") != std::string::npos;
    std::string role = tpl;
    replace_all(role, "{language}", cfg.language);
    replace_all(role, "{tone}", cfg.tone);
    replace_all(role, "{max_words}",
                cfg.max_words > 0 ? std::to_string(cfg.max_words) : "");
    // Вырезаем маркеры статьи (и их подписи), чтобы в системном промпте не
    // висел пустой «Заголовок: ».
    replace_all(role, "Заголовок: {title}", "");
    replace_all(role, "Текст: {body}", "");
    replace_all(role, "{title}", "");
    replace_all(role, "{body}", "");
    role = trim_right(collapse_blank_lines(role));
    if (with_max_words && cfg.max_words > 0 && !had_max_words_ph) {
        role += "\n\nОбъём: примерно " + std::to_string(cfg.max_words) + " слов.";
    }
    // SEO: статья начинается с детального вступления-пересказа (жирный лид),
    // а ключевая фраза темы (focus_keyword) уходит именно туда, а НЕ в заголовок.
    role +=
        "\n\nНачни рерайт с небольшого вступления — краткого пересказа сути новости "
        "(3–4 предложения) в самом первом абзаце. Оформи вступление жирным шрифтом "
        "(markdown **…**), НЕ делай из него заголовок (никаких строк, начинающихся "
        "с '#' или '##'). Не используй в тексте служебные слова вроде «вступление», "
        "«пересказ», «краткий обзор» — просто дай текст пересказа. "
        "Для SEO-оптимизации органично "
        "используй ключевую фразу по теме статьи (например, главную мысль из "
        "заголовка) обязательно в этом вступительном первом абзаце и желательно в "
        "одном из подзаголовков (строки '## '), с умеренной плотностью (около 1%). "
        "Ключевую фразу НЕ ставь ни в заголовок статьи, ни в seo_title — она должна "
        "быть только во вступлении. Не допускай злоупотребления повторами и не искажай факты.";
    return role;
}

std::string build_role_prompt(const RewriteConfig& cfg) {
    return build_role_from_template(cfg.prompt_template, cfg, /*with_max_words=*/true);
}

std::string build_user_prompt(const Article& src, const RewriteConfig& cfg) {
    std::string p = "Заголовок: " + src.title_original + "\nТекст: " +
           truncate_input(src.body_original, cfg.max_input_chars);
    if (!src.author_original.empty()) {
        p += "\nАвтор оригинала: " + src.author_original;
        if (has_cyrillic(src.author_original)) {
            // Имя уже на кириллице — подпись проставит код, модель не дублирует.
            p += "\nПодпись «Автор оригинала» в конце текста НЕ добавляй — она "
                 "проставляется автоматически.";
        } else {
            // Китайские/латинские имена: модель транслитерирует в скобках.
            p += "\nВ самом конце текста добавь одну строку-подпись: "
                 "«Автор оригинала: " + src.author_original +
                 " (<кириллическая транслитерация имени)».";
        }
    }
    return p;
}

std::string build_prompt(const Article& src, const RewriteConfig& cfg) {
    // Для совместимости/тестов — полный промпт как раньше (роль + контент).
    return build_role_prompt(cfg) + "\n\n" + build_user_prompt(src, cfg);
}

namespace {

// Обрезка строки и схлопывание пробелов (как в extractor).
std::string trim_collapse(const std::string& s) {
    const std::size_t b = s.find_first_not_of(" \t\r\n");
    const std::size_t e = s.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    std::string line = s.substr(b, e - b + 1);
    std::string out;
    out.reserve(line.size());
    bool prev_space = false;
    for (char c : line) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            prev_space = true;
        } else {
            if (prev_space && !out.empty()) out += ' ';
            out += c;
            prev_space = false;
        }
    }
    return out;
}

} // namespace

// Обрезка строки ДО max_chars СИМВОЛОВ (code points, корректно с UTF-8), а не
// байт. Если режем посреди слова — откатываемся к последнему пробелу; при
// необходимости добавляем суффикс (напр. "…"). Пустой/короткий — без изменений.
std::string truncate_chars(const std::string& s, std::size_t max_chars,
                           bool add_ellipsis) {
    // Индексы начала каждого code point.
    std::vector<std::size_t> off;
    off.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        off.push_back(i);
        const unsigned char b0 = static_cast<unsigned char>(s[i]);
        int adv = 1;
        if (b0 >= 0x80) {
            if ((b0 & 0xE0) == 0xC0) adv = 2;
            else if ((b0 & 0xF0) == 0xE0) adv = 3;
            else if ((b0 & 0xF8) == 0xF0) adv = 4;
        }
        i += adv;
    }
    if (off.size() <= max_chars) return s;

    std::size_t cut = max_chars;            // индекс в off
    while (cut > 0) {                       // откат к границе слова
        const std::size_t st = off[cut - 1];
        const char c = s[st];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') break;
        --cut;
    }
    if (cut == 0) cut = max_chars;         // пробела нет — жёсткая обрезка
    std::string out = s.substr(0, off[cut]);
    if (add_ellipsis) out += "…";
    return out;
}

// Нижний регистр с учётом кириллицы (std::tolower только для ASCII).
std::string lower_utf8_str(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char b0 = static_cast<unsigned char>(s[i]);
        unsigned int cp = 0;
        int adv = 1;
        if (b0 < 0x80) { cp = b0; adv = 1; }
        else if ((b0 & 0xE0) == 0xC0) {
            cp = b0 & 0x1F; adv = 2;
            if (i + 1 < s.size()) cp = (cp << 6) | (s[i + 1] & 0x3F);
        } else if ((b0 & 0xF0) == 0xE0) {
            cp = b0 & 0x0F; adv = 3;
            if (i + 1 < s.size()) cp = (cp << 6) | (s[i + 1] & 0x3F);
            if (i + 2 < s.size()) cp = (cp << 6) | (s[i + 2] & 0x3F);
        } else if ((b0 & 0xF8) == 0xF0) {
            cp = b0 & 0x07; adv = 4;
            if (i + 1 < s.size()) cp = (cp << 6) | (s[i + 1] & 0x3F);
            if (i + 2 < s.size()) cp = (cp << 6) | (s[i + 2] & 0x3F);
            if (i + 3 < s.size()) cp = (cp << 6) | (s[i + 3] & 0x3F);
        } else { cp = b0; adv = 1; }
        if (cp >= 0x410 && cp <= 0x42F) cp += 0x20;  // заглавная кириллица
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        i += adv;
    }
    return out;
}

// Приведение ключевой фразы к норме: нижний регистр + не более 4 слов.
std::string normalize_keyword(const std::string& kw) {
    std::string low = lower_utf8_str(kw);
    std::string token, out;
    int words = 0;
    auto flush = [&]() {
        if (token.empty()) return;
        if (words >= 4) { token.clear(); return; }  // ограничение длины фразы
        if (words > 0) out += ' ';
        out += token;
        ++words;
        token.clear();
    };
    for (char c : low) {
        if (std::isspace(static_cast<unsigned char>(c))) flush();
        else token += c;
    }
    flush();
    return out;
}

// --- Эвристики «деградации» ответа LLM --------------------------------------
// Перегруженная модель (после rate-limit, код 1305) может вернуть 200 с
// мусорным/каноническим текстом вместо реального рерайта. Ниже — дешёвые
// проверки, отсеивающие наиболее частые случаи такого выхлопа.

namespace {

// Итератор по Unicode code points (корректный разбор UTF-8).
bool next_cp(const std::string& s, std::size_t& i, unsigned int& cp) {
    if (i >= s.size()) return false;
    const unsigned char b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) { cp = b0; i += 1; return true; }
    int n = 0;
    if ((b0 & 0xE0) == 0xC0) { n = 1; cp = b0 & 0x1F; }
    else if ((b0 & 0xF0) == 0xE0) { n = 2; cp = b0 & 0x0F; }
    else if ((b0 & 0xF8) == 0xF0) { n = 3; cp = b0 & 0x07; }
    else { cp = b0; i += 1; return true; }  // некорректный лидер — байт как есть
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

enum class Script { Unknown, Latin, Cyrillic, Han, Other };

Script script_of_cp(unsigned int cp) {
    if (cp <= 0x024F) {
        if (cp == 0x00A0) return Script::Unknown;  // неразр. пробел
        return Script::Latin;
    }
    if ((cp >= 0x0400 && cp <= 0x04FF) || (cp >= 0x0500 && cp <= 0x052F))
        return Script::Cyrillic;
    if ((cp >= 0x3040 && cp <= 0x30FF) ||   // японские слоговые азбуки
        (cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK Ext A
        (cp >= 0x4E00 && cp <= 0x9FFF))     // CJK Unified Ideographs
        return Script::Han;
    return Script::Other;
}

double script_ratio(const std::string& s, Script want) {
    unsigned long wantc = 0, letters = 0;
    std::size_t i = 0;
    unsigned int cp = 0;
    while (next_cp(s, i, cp)) {
        const Script sc = script_of_cp(cp);
        if (sc == Script::Unknown) continue;
        ++letters;
        if (sc == want) ++wantc;
    }
    return letters ? static_cast<double>(wantc) / static_cast<double>(letters) : 0.0;
}

Script expected_script(const std::string& lang) {
    std::string l;
    l.reserve(lang.size());
    for (char c : lang) l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (l == "ru" || l == "русский" || l == "russian") return Script::Cyrillic;
    if (l == "en" || l == "english" || l == "английский" || l == "es" ||
        l == "fr" || l == "de" || l == "it" || l == "pt" || l == "испанский" ||
        l == "французский" || l == "немецкий")
        return Script::Latin;
    if (l == "zh" || l == "китайский" || l == "chinese" || l == "中文" ||
        l == "ja" || l == "японский" || l == "japanese")
        return Script::Han;
    return Script::Unknown;  // неизвестный язык — не проверяем
}

bool ci_contains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    std::string h, n;
    h.reserve(hay.size());
    n.reserve(needle.size());
    for (char c : hay) h += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char c : needle) n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h.find(n) != std::string::npos;
}

bool looks_like_refusal(const std::string& text) {
    static const char* kPhrases[] = {
        "i cannot", "i can't", "as an ai", "as a language model",
        "i am a language model", "i'm a language model", "i am sorry",
        "i'm sorry", "i cannot help", "я не могу", "извините",
        "как языковая модель", "как искусственный интеллект",
        "в качестве языковой модели", "не имею доступа", "у меня нет доступа",
        "я не имею информации", "i don't have access", "my knowledge",
        "модель перегружена", "превышен лимит", "rate limit",
        "пожалуйста, попробуйте позже", "попробуйте позже", "i am unable",
        "unable to", "я не могу помочь", "the model is overloaded",
    };
    for (const char* p : kPhrases) {
        if (ci_contains(text, p)) return true;
    }
    return false;
}

} // namespace

bool validate_rewrite(const std::string& title, const std::string& body,
                      const std::string& expected_lang, std::string& error) {
    const std::string combined = title + "\n" + body;
    if (looks_like_refusal(combined)) {
        error = "ответ LLM похож на отказ/системное сообщение (деградация модели)";
        return false;
    }
    const Script want = expected_script(expected_lang);
    if (want != Script::Unknown) {
        // Проверяем основной объём (тело) либо заголовок, если тело пусто.
        const std::string& sample = body.empty() ? title : body;
        const double r = script_ratio(sample, want);
        if (r < 0.5) {
            error = "язык ответа не соответствует заданному (" + expected_lang +
                    "): похоже на деградацию модели";
            return false;
        }
    }
    return true;
}

RewriteResult parse_response(const std::string& response) {
    RewriteResult result;
    if (response.empty()) {
        result.error = "пустой ответ LLM";
        return result;
    }

    // Собираем непустые строки (со схлопыванием пробелов) для анализа.
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < response.size()) {
        const std::size_t nl = response.find('\n', start);
        const std::string line = response.substr(
            start, nl == std::string::npos ? std::string::npos : nl - start);
        const std::string clean = trim_collapse(line);
        if (!clean.empty()) lines.push_back(clean);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }

    if (lines.empty()) {
        result.error = "пустой ответ LLM";
        return result;
    }

    // Первая строка — заголовок, ТОЛЬКО если она похожа на заголовок
    // (достаточно короткая). Модель часто не отделяет заголовок от тела и
    // выдаёт первой строкой целый абзац текста — особенно при деградации
    // ответа после rate-limit (см. лог: «Технофашисты XXI века»). Такую
    // слишком длинную «псевдо-заголовку» считаем частью тела, а заголовок
    // оставляем пустым — вызывающий (worker::rewrite) подставит оригинальный.
    const bool first_is_title = lines.front().size() <= 150;
    result.ok = true;
    if (first_is_title) {
        result.title = lines.front();
        for (std::size_t i = 1; i < lines.size(); ++i) {
            if (!result.body.empty()) result.body += '\n';
            result.body += lines[i];
        }
    } else {
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (!result.body.empty()) result.body += '\n';
            result.body += lines[i];
        }
    }
    return result;
}

RewriteResult rewrite_article(const Article& src, const RewriteConfig& cfg,
                              const std::string& role_prompt, const LlmFn& llm) {
    RewriteResult result;
    if (!llm) {
        result.error = "LLM не настроен";
        return result;
    }

    const std::string user = build_user_prompt(src, cfg);
    std::string response;
    std::string llm_error;
    if (!llm(role_prompt, user, response, llm_error)) {
        result.error = llm_error.empty() ? "LLM недоступен" : llm_error;
        return result;
    }

    result = parse_response(response);
    if (result.ok && !validate_rewrite(result.title, result.body,
                                        cfg.language, result.error)) {
        result.ok = false;
    }
    return result;
}

RewriteResult rewrite_article(const Article& src, const RewriteConfig& cfg,
                              const LlmFn& llm) {
    return rewrite_article(src, cfg, build_role_prompt(cfg), llm);
}

std::string build_seo_role_prompt(const SeoConfig& cfg, const std::string& language) {
    std::string role = cfg.prompt_template;
    replace_all(role, "{language}", language);
    // Маркеры статьи вырезаются — контент приходит в user-сообщении.
    replace_all(role, "Заголовок: {title}", "");
    replace_all(role, "Текст: {body}", "");
    replace_all(role, "{title}", "");
    replace_all(role, "{body}", "");
    return trim_right(collapse_blank_lines(role));
}

std::string build_seo_user_prompt(const Article& src, const SeoConfig& cfg) {
    (void)cfg;
    return "Язык: " + src.language + "\nЗаголовок: " + src.title_rewritten +
           "\nТекст: " + src.body_rewritten;
}

std::string build_seo_prompt(const Article& src, const SeoConfig& cfg) {
    // Для совместимости/тестов — роль + контент.
    return build_seo_role_prompt(cfg, src.language) + "\n\n" +
           build_seo_user_prompt(src, cfg);
}

// Вырезать ключевую фразу из заголовка (case-insensitive), чтобы модель не
// включала её в seo_title/заголовок (ключ должен быть во вступлении, а не в
// заголовке). Убирает также «висячие» разделители, оставшиеся после удаления.
static std::string strip_keyphrase_from_title(const std::string& title,
                                             const std::string& kp) {
    if (title.empty() || kp.empty()) return title;
    const std::string low_title = lower_utf8_str(title);
    const std::string low_kp = lower_utf8_str(kp);
    std::string out = title;
    std::string out_low = low_title;
    std::size_t pos;
    while ((pos = out_low.find(low_kp)) != std::string::npos) {
        out.erase(pos, kp.size());
        out_low.erase(pos, low_kp.size());
    }
    // Срезать ведущие пробелы и один разделитель (":", "-", ",", "|") + пробел.
    std::size_t b = out.find_first_not_of(" \t\r\n");
    if (b != std::string::npos) out = out.substr(b);
    if (!out.empty() && (out[0] == ':' || out[0] == '-' || out[0] == ',' ||
                         out[0] == '|')) {
        out.erase(0, 1);
        if (!out.empty() && (out[0] == ' ' || out[0] == '\t')) out.erase(0, 1);
    }
    if (out.compare(0, 3, "— ") == 0) out = out.substr(3);  // em-dash + space
    // Аналогично хвостовые разделители.
    std::size_t e = out.find_last_not_of(" \t\r\n:-,|");
    if (e != std::string::npos) out = out.substr(0, e + 1);
    return trim_collapse(out);
}

SeoResult parse_seo_response(const std::string& response) {
    SeoResult result;
    if (response.empty()) {
        result.error = "пустой ответ LLM";
        return result;
    }
    const std::size_t open = response.find('{');
    const std::size_t close = response.rfind('}');
    if (open == std::string::npos || close == std::string::npos || close <= open) {
        result.error = "в ответе LLM нет JSON";
        return result;
    }
    const std::string json = response.substr(open, close - open + 1);
    bool ok = false;
    Json j = Json::parse(json, &ok);
    if (!ok || !j.is_object()) {
        result.error = "не удалось разобрать JSON SEO";
        return result;
    }
    result.focus_keyword = normalize_keyword(j.get("focus_keyword").as_string());
    result.meta_description = trim_collapse(j.get("meta_description").as_string());
    result.seo_title = trim_collapse(j.get("seo_title").as_string());
    result.seo_title = strip_keyphrase_from_title(result.seo_title, result.focus_keyword);
    // Жёсткие ограничения длины (нормы Yoast как отправная точка):
    //   - seo_title   ≤ 60 символов (обрезка по границе слова + "…");
    //   - description ≤ 160 символов (обрезка по границе слова + "…");
    //   - keyword уже приведён к 2-4 словам и нижнему регистру.
    if (result.seo_title.size() > 60)
        result.seo_title = truncate_chars(result.seo_title, 60, true);
    if (result.meta_description.size() > 160)
        result.meta_description = truncate_chars(result.meta_description, 160, true);
    if (result.focus_keyword.empty() && result.meta_description.empty() &&
        result.seo_title.empty()) {
        result.error = "SEO-JSON пуст";
        return result;
    }
    // Оптимальный slug из ключевой фразы (транслит RU→EN + вырезка стоп-слов).
    if (!result.focus_keyword.empty())
        result.seo_slug = make_slug(result.focus_keyword);
    result.ok = true;
    return result;
}

SeoResult generate_seo(const Article& src, const SeoConfig& cfg,
                        const std::string& role_prompt, const LlmFn& llm) {
    SeoResult result;
    if (!llm) {
        result.error = "LLM не настроен";
        return result;
    }
    if (src.title_rewritten.empty() || src.body_rewritten.empty()) {
        result.error = "нет рерайта для SEO";
        return result;
    }

    const std::string user = build_seo_user_prompt(src, cfg);
    std::string response;
    std::string llm_error;
    if (!llm(role_prompt, user, response, llm_error)) {
        result.error = llm_error.empty() ? "LLM недоступен" : llm_error;
        return result;
    }
    return parse_seo_response(response);
}

SeoResult generate_seo(const Article& src, const SeoConfig& cfg,
                        const LlmFn& llm) {
    return generate_seo(src, cfg, build_seo_role_prompt(cfg, src.language), llm);
}

std::string build_combined_role_prompt(const RewriteConfig& cfg) {
    // Роль = инструкции рерайта (те же подстановки, что в build_role_prompt,
    // БЕЗ маркеров статьи) + СТРОГИЙ JSON-формат выхода. Всё это статично для
    // обхода и шлётся модели РОВНО ОДИН раз.
    std::string role = build_role_from_template(cfg.prompt_template, cfg,
                                                 /*with_max_words=*/false);
    role +=
        "\n\nОтветь СТРОГО одним JSON-объектом, без какого-либо текста до или после "
        "него и без markdown-разметки (никаких ```json). Только ключи ниже:\n"
        "{\n"
        "  \"title\": \"переписанный заголовок новости\",\n"
        "  \"body\": \"полный переписанный текст новости (только проза, без JSON)\",\n"
        "  \"focus_keyword\": \"ключевая фраза 2-4 слова, строго нижний регистр, на языке статьи\",\n"
        "  \"meta_description\": \"одно предложение, 150-160 символов, суть новости\",\n"
        "  \"seo_title\": \"SEO-заголовок до 60 символов: яркий и точный, ключевую фразу НЕ включай (она во вступлении)\"\n"
        "}\n"
        "Поля focus_keyword/meta_description/seo_title — SEO-метаданные; если не "
        "уверены, оставьте пустую строку. Поле body должно содержать ТОЛЬКО текст "
        "рерайта.";
    return role;
}

std::string build_combined_user_prompt(const Article& src, const RewriteConfig& cfg) {
    return build_user_prompt(src, cfg);
}

std::string build_combined_prompt(const Article& src, const RewriteConfig& cfg) {
    // Для совместимости/тестов — роль + контент.
    return build_combined_role_prompt(cfg) + "\n\n" +
           build_combined_user_prompt(src, cfg);
}

RewriteSeoResult parse_rewrite_seo_response(const std::string& response) {
    RewriteSeoResult result;
    if (response.empty()) {
        result.error = "пустой ответ LLM";
        return result;
    }

    const std::size_t open = response.find('{');
    const std::size_t close = response.rfind('}');
    if (open == std::string::npos || close == std::string::npos || close <= open) {
        result.error = "в ответе LLM нет JSON (ожидался рерайт+SEO в JSON)";
        return result;
    }
    const std::string json = response.substr(open, close - open + 1);
    bool ok = false;
    Json j = Json::parse(json, &ok);
    if (!ok || !j.is_object()) {
        result.error = "не удалось разобрать JSON рерайта+SEO";
        return result;
    }

    result.title = trim_collapse(j.get("title").as_string());
    result.body = trim_collapse(j.get("body").as_string());

    // SEO-поля опциональны: их отсутствие не портит рерайт.
    result.seo.focus_keyword = trim_collapse(j.get("focus_keyword").as_string());
    result.seo.meta_description = trim_collapse(j.get("meta_description").as_string());
    result.seo.seo_title = trim_collapse(j.get("seo_title").as_string());
    result.seo.seo_title = strip_keyphrase_from_title(result.seo.seo_title,
                                                      result.seo.focus_keyword);
    if (!result.seo.focus_keyword.empty() ||
        !result.seo.meta_description.empty() ||
        !result.seo.seo_title.empty()) {
        result.seo.ok = true;
    }

    if (result.title.empty() || result.body.empty()) {
        result.error = "в JSON нет заголовка или тела рерайта";
        return result;
    }
    result.ok = true;
    return result;
}

RewriteSeoResult rewrite_and_seo(const Article& src, const RewriteConfig& cfg,
                                  const std::string& role_prompt, const LlmFn& llm) {
    RewriteSeoResult result;
    if (!llm) {
        result.error = "LLM не настроен";
        return result;
    }

    const std::string user = build_combined_user_prompt(src, cfg);
    std::string response;
    std::string llm_error;
    if (!llm(role_prompt, user, response, llm_error)) {
        result.error = llm_error.empty() ? "LLM недоступен" : llm_error;
        return result;
    }
    result = parse_rewrite_seo_response(response);
    if (result.ok && !validate_rewrite(result.title, result.body,
                                        cfg.language, result.error)) {
        result.ok = false;
    }
    return result;
}

RewriteSeoResult rewrite_and_seo(const Article& src, const RewriteConfig& cfg,
                                   const LlmFn& llm) {
    return rewrite_and_seo(src, cfg, build_combined_role_prompt(cfg), llm);
}

// ============================================================================
// Phase 3 — LLM-доводка (фидбек-скоркард)
// ============================================================================

std::string seo_feedback_text(const SeoReport& rep) {
    std::string out;
    for (const auto& m : rep.metrics) {
        if (m.status == SeoStatus::Poor) {
            if (!out.empty()) out += "\n";
            out += "- " + m.label + ": сейчас " + m.text + ".";
        }
    }
    return out;
}

std::string build_seo_refine_role_prompt(const SeoConfig& cfg,
                                         const std::string& language) {
    (void)cfg;
    (void)language;
    return "Ты — опытный SEO-редактор. Тебе дана уже переписанная новость и список "
           "проблем по SEO-копирайту. Перепиши ТОЛЬКО проблемные места, сохранив все "
           "факты, смысл и стиль исходного текста. Обязательно сохраняй структуру с "
           "подзаголовками (строки, начинающиеся с '## '). Не выдумывай и не убирай "
           "факты. Верни ТОЛЬКО исправленный текст статьи в markdown, без пояснений и "
           "без обёртки в код (никаких ```).";
}

std::string build_seo_refine_user_prompt(const Article& src,
                                         const std::string& feedback,
                                         const SeoConfig& cfg) {
    (void)cfg;
    std::string out = "Язык: " + src.language + "\n";
    if (!src.seo_focus_keyword.empty())
        out += "Ключевая фраза: " + src.seo_focus_keyword + "\n";
    out += "\nПроблемы (устрани их, не трогая остальной текст):\n";
    out += feedback.empty()
               ? "(проблем не выявлено — верни текст без изменений)"
               : feedback;
    out += "\n\nТекст статьи:\n" + src.body_rewritten;
    return out;
}

namespace {
// Обрезка ТОЛЬКО краёв (без схлопывания внутренних пробелов/переводов строк) —
// чтобы сохранить структуру markdown-тела (подзаголовки, абзацы).
std::string trim_only(const std::string& s) {
    const std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const std::size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Снимает возможную обёртку ```markdown … ``` / ``` … ``` с ответа модели,
// сохраняя внутреннюю разбивку на абзацы/подзаголовки.
std::string strip_fence(const std::string& s) {
    std::string t = trim_only(s);
    if (t.rfind("```", 0) == 0) {
        std::size_t end = t.rfind("```");
        if (end > 3) {
            std::size_t start = 3;
            // пропускаем имя языка после открывающих ```
            while (start < t.size() && t[start] != '\n') ++start;
            if (start < t.size()) ++start;  // съедаем '\n'
            t = t.substr(start, end - start);
            t = trim_only(t);
        }
    }
    return t;
}
} // namespace

SeoRefineResult parse_seo_refine_response(const std::string& response,
                                          const std::string& expected_lang) {
    SeoRefineResult r;
    if (response.empty()) {
        r.error = "пустой ответ LLM";
        return r;
    }
    std::string body = strip_fence(response);
    if (looks_like_refusal(body)) {
        r.error = "ответ LLM похож на отказ/системное сообщение";
        return r;
    }
    std::string verr;
    if (!validate_rewrite("", body, expected_lang, verr)) {
        r.error = verr;
        return r;
    }
    r.body = body;
    r.ok = true;
    return r;
}

SeoRefineResult seo_refine(const Article& src, const SeoConfig& cfg,
                           const std::string& feedback,
                           const std::string& role_prompt, const LlmFn& llm) {
    SeoRefineResult r;
    if (!llm) {
        r.error = "LLM не настроен";
        return r;
    }
    if (src.body_rewritten.empty()) {
        r.error = "нет текста для доводки";
        return r;
    }
    const std::string user = build_seo_refine_user_prompt(src, feedback, cfg);
    std::string response, llm_error;
    if (!llm(role_prompt, user, response, llm_error)) {
        r.error = llm_error.empty() ? "LLM недоступен" : llm_error;
        return r;
    }
    SeoRefineResult parsed = parse_seo_refine_response(response, src.language);
    if (!parsed.ok) return parsed;
    // Защита от усечения/галлюцинации: доводка не должна радикально менять объём.
    const double ratio = static_cast<double>(parsed.body.size()) /
                         std::max<std::size_t>(src.body_rewritten.size(), 1);
    if (ratio < 0.5 || ratio > 1.8) {
        r.error = "LLM-доводка сильно изменила объём текста (×" +
                  std::to_string(static_cast<int>(ratio * 100) / 100.0) +
                  "), отклоняем";
        return r;
    }
    r.body = parsed.body;
    r.ok = true;
    return r;
}

SeoRefineResult seo_refine(const Article& src, const SeoConfig& cfg,
                           const std::string& feedback, const LlmFn& llm) {
    return seo_refine(src, cfg, feedback,
                      build_seo_refine_role_prompt(cfg, src.language), llm);
}

} // namespace news_rewriter
