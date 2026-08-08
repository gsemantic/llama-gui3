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
    std::size_t start = 0;
    while (start < response.size()) {
        const std::size_t nl = response.find('\n', start);
        const std::string line = response.substr(
            start, nl == std::string::npos ? std::string::npos : nl - start);
        const std::string clean = trim_collapse(line);
        if (!clean.empty()) {
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

} // namespace news_rewriter
