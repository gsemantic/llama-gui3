#pragma once

#include <functional>
#include <string>

#include "common.h"
#include "config.h"

namespace news_rewriter {

// Результат рерайта статьи.
struct RewriteResult {
    bool ok = false;
    std::string title;
    std::string body;
    std::string error;
};

// Обёртка над llm_complete: вызывается ТОЛЬКО в worker-потоке.
// Принимает промпт, возвращает ответ и текст ошибки (при сбое).
using LlmFn = std::function<bool(const std::string& prompt,
                                 std::string& response, std::string& error)>;

// Сборка промпта по шаблону config.rewrite.prompt_template с подстановками
// {title}, {body}, {language}, {tone}, {max_words}. При max_words > 0 в конец
// промпта добавляется инструкция о примерном объёме статьи.
std::string build_prompt(const Article& src, const RewriteConfig& cfg);

// Разбор ответа LLM: первая непустая строка — заголовок, остальное — тело.
// Один длинный абзац без разделения считается телом новости (заголовок пуст).
RewriteResult parse_response(const std::string& response);

// Полный рерайт: промпт → llm → разбор ответа. Ошибки LLM не бросают
// исключений — возвращаются в RewriteResult.
RewriteResult rewrite_article(const Article& src, const RewriteConfig& cfg,
                              const LlmFn& llm);

} // namespace news_rewriter
