#include "rewriter.h"

#include <cctype>

namespace news_rewriter {

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
    replace("{body}", src.body_original);
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

RewriteResult parse_response(const std::string& response) {
    RewriteResult result;
    if (response.empty()) {
        result.error = "пустой ответ LLM";
        return result;
    }

    // Первая непустая строка — заголовок, остальное — тело.
    std::string first;
    std::string body;
    int non_empty_lines = 0;
    std::size_t start = 0;
    while (start < response.size()) {
        const std::size_t nl = response.find('\n', start);
        const std::string line = response.substr(
            start, nl == std::string::npos ? std::string::npos : nl - start);
        const std::string clean = trim_collapse(line);
        if (!clean.empty()) {
            ++non_empty_lines;
            if (first.empty()) {
                first = clean;
            } else if (body.empty()) {
                body = clean;
            } else {
                body += "\n" + clean;
            }
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }

    if (first.empty()) {
        result.error = "пустой ответ LLM";
        return result;
    }

    // Модель иногда возвращает весь текст рерайта одним абзацем, не разделяя
    // заголовок и тело. Одинокую длинную строку считаем телом новости, а не
    // заголовком (заголовок в этом случае останется оригинальным).
    if (non_empty_lines == 1 && first.size() > 150) {
        result.ok = true;
        result.body = first;
        return result;
    }

    result.ok = true;
    result.title = first;
    result.body = body;
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
    return parse_rewrite_seo_response(response);
}

} // namespace news_rewriter
