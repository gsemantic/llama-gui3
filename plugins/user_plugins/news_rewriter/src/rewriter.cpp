#include "rewriter.h"

#include <cctype>

namespace news_rewriter {

// Обрезка текста до max байт с учётом многобайтовой (UTF-8) кодировки:
// не разрываем символ посередине (откатываемся за продолжения 0x80..0xBF).
std::string truncate_input(const std::string& s, int max) {
    if (max <= 0 || static_cast<int>(s.size()) <= max) return s;
    std::size_t cut = static_cast<std::size_t>(max);
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

std::string build_prompt(const Article& src, const RewriteConfig& cfg) {
    std::string prompt = cfg.prompt_template;
    const auto replace = [&prompt](const std::string& from, const std::string& to) {
        std::size_t pos = 0;
        while ((pos = prompt.find(from, pos)) != std::string::npos) {
            prompt.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replace("{title}", src.title_original);
    replace("{body}", truncate_input(src.body_original, cfg.max_input_chars));
    replace("{language}", cfg.language);
    replace("{tone}", cfg.tone);
    replace("{max_words}", cfg.max_words > 0 ? std::to_string(cfg.max_words) : "");
    if (cfg.max_words > 0) {
        prompt += "\n\nОбъём: примерно " + std::to_string(cfg.max_words) + " слов.";
    }
    return prompt;
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
                              const LlmFn& llm) {
    RewriteResult result;
    if (!llm) {
        result.error = "LLM не настроен";
        return result;
    }

    const std::string prompt = build_prompt(src, cfg);
    std::string response;
    std::string llm_error;
    if (!llm(prompt, response, llm_error)) {
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

std::string build_seo_prompt(const Article& src, const SeoConfig& cfg) {
    std::string prompt = cfg.prompt_template;
    const auto replace = [&prompt](const std::string& from, const std::string& to) {
        std::size_t pos = 0;
        while ((pos = prompt.find(from, pos)) != std::string::npos) {
            prompt.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replace("{title}", src.title_rewritten);
    replace("{body}", src.body_rewritten);
    replace("{language}", src.language);
    return prompt;
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
    result.focus_keyword = trim_collapse(j.get("focus_keyword").as_string());
    result.meta_description = trim_collapse(j.get("meta_description").as_string());
    result.seo_title = trim_collapse(j.get("seo_title").as_string());
    if (result.focus_keyword.empty() && result.meta_description.empty() &&
        result.seo_title.empty()) {
        result.error = "SEO-JSON пуст";
        return result;
    }
    result.ok = true;
    return result;
}

SeoResult generate_seo(const Article& src, const SeoConfig& cfg,
                        const LlmFn& llm) {
    SeoResult result;
    if (!llm) {
        result.error = "LLM не настроен";
        return result;
    }
    if (src.title_rewritten.empty() || src.body_rewritten.empty()) {
        result.error = "нет рерайта для SEO";
        return result;
    }

    const std::string prompt = build_seo_prompt(src, cfg);
    std::string response;
    std::string llm_error;
    if (!llm(prompt, response, llm_error)) {
        result.error = llm_error.empty() ? "LLM недоступен" : llm_error;
        return result;
    }
    return parse_seo_response(response);
}

std::string build_combined_prompt(const Article& src, const RewriteConfig& cfg) {
    // Базовый рерайт-промпт (те же подстановки, что в build_prompt), но выход
    // переопределяется на строгий JSON (см. ниже).
    std::string prompt = cfg.prompt_template;
    const auto replace = [&prompt](const std::string& from, const std::string& to) {
        std::size_t pos = 0;
        while ((pos = prompt.find(from, pos)) != std::string::npos) {
            prompt.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replace("{title}", src.title_original);
    replace("{body}", src.body_original);
    replace("{language}", cfg.language);
    replace("{tone}", cfg.tone);
    replace("{max_words}", cfg.max_words > 0 ? std::to_string(cfg.max_words) : "");
    if (cfg.max_words > 0) {
        prompt += "\n\nОбъём: примерно " + std::to_string(cfg.max_words) + " слов.";
    }

    prompt +=
        "\n\nОтветь СТРОГО одним JSON-объектом, без какого-либо текста до или после "
        "него и без markdown-разметки (никаких ```json). Только ключи ниже:\n"
        "{\n"
        "  \"title\": \"переписанный заголовок новости\",\n"
        "  \"body\": \"полный переписанный текст новости (только проза, без JSON)\",\n"
        "  \"focus_keyword\": \"ключевая фраза 2-4 слова, строго нижний регистр, на языке статьи\",\n"
        "  \"meta_description\": \"одно предложение, 150-160 символов, суть новости\",\n"
        "  \"seo_title\": \"SEO-заголовок до 60 символов с ключевым словом в начале\"\n"
        "}\n"
        "Поля focus_keyword/meta_description/seo_title — SEO-метаданные; если не "
        "уверены, оставьте пустую строку. Поле body должно содержать ТОЛЬКО текст "
        "рерайта.";
    return prompt;
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
                                 const LlmFn& llm) {
    RewriteSeoResult result;
    if (!llm) {
        result.error = "LLM не настроен";
        return result;
    }

    const std::string prompt = build_combined_prompt(src, cfg);
    std::string response;
    std::string llm_error;
    if (!llm(prompt, response, llm_error)) {
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

} // namespace news_rewriter
