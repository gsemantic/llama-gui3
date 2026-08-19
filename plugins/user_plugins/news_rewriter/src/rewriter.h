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

// Проверка, что ответ LLM не «деградировал» (отказ/системное сообщение модели,
// выдача не на заданном языке и т.п.). Возвращает false и заполняет error, если
// ответ похож на мусорный выхлоп перегруженной модели — чтобы воркер не
// опубликовал сгенерированный от балды текст (см. деградацию после rate-limit).
// expected_lang — целевой язык рерайта (cfg.rewrite.language), по нему
// проверяется письменность выдачи (ru→кириллица, en→латиница, zh→иероглифы).
bool validate_rewrite(const std::string& title, const std::string& body,
                      const std::string& expected_lang, std::string& error);

// Полный рерайт: промпт → llm → разбор ответа. Ошибки LLM не бросают
// исключений — возвращаются в RewriteResult.
RewriteResult rewrite_article(const Article& src, const RewriteConfig& cfg,
                              const LlmFn& llm);

// Результат авто-SEO (отдельный JSON-запрос к LLM).
struct SeoResult {
    bool ok = false;
    std::string error;
    std::string focus_keyword;
    std::string meta_description;
    std::string seo_title;
};

// Сборка SEO-промпта по шаблону config.rewrite.seo.prompt_template
// с подстановками {title}, {body}, {language}.
std::string build_seo_prompt(const Article& src, const SeoConfig& cfg);

// Разбор ответа LLM: ищем первый '{' ... последний '}' и парсим JSON
// {focus_keyword, meta_description, seo_title} (устойчиво к markdown- fences).
SeoResult parse_seo_response(const std::string& response);

// Генерация SEO-данных: промпт → llm → разбор. Best-effort: при сбое
// LLM/парсинга возвращает ok=false (вызывающий решает, прервать ли статью).
SeoResult generate_seo(const Article& src, const SeoConfig& cfg,
                       const LlmFn& llm);

// Комбинированный результат рерайта + SEO (один запрос к LLM).
struct RewriteSeoResult {
    bool ok = false;            // true, если получен валидный title+body
    std::string title;
    std::string body;
    std::string error;          // текст ошибки LLM/парсинга (при !ok)
    SeoResult seo;              // SEO-поля (опциональны: могут быть пустыми)
};

// Сборка КОМБИНИРОВАННОГО промпта: рерайт и SEO в одном ответе. Модель должна
// вернуть СТРОГО один JSON-объект {title, body, focus_keyword,
// meta_description, seo_title} без текста до/после и без markdown-разметки.
// title+body — обязательны (сам рерайт); SEO-поля опциональны.
std::string build_combined_prompt(const Article& src, const RewriteConfig& cfg);

// Разбор комбинированного ответа LLM. Ищет первый '{' … последний '}' и
// парсит JSON. title+body обязательны для ok; отсутствие SEO-полей не портит
// рерайт (seo.ok=false — статью публикуем без SEO-мета).
RewriteSeoResult parse_rewrite_seo_response(const std::string& response);

// Рерайт + SEO ОДНИМ вызовом LLM (экономит облачные запросы/лимиты).
// При неудаче (сбой LLM/парсинга) ok=false; вызывающий может откатиться к
// двум отдельным вызовам.
RewriteSeoResult rewrite_and_seo(const Article& src, const RewriteConfig& cfg,
                                 const LlmFn& llm);

} // namespace news_rewriter
