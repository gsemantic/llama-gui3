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
// Принимает системный промпт (роль, статичен для всего обхода) и
// пользовательское сообщение (контент одной статьи), возвращает ответ и
// текст ошибки (при сбое). Разделение позволяет плагину отправлять тяжёлые
// инструкции/роль РОВНО ОДИН раз (через llm_complete_ex), а не дублировать их
// для каждой новости в рамках одного обхода (см. «промпт-роль»).
using LlmFn = std::function<bool(const std::string& system,
                                 const std::string& user,
                                 std::string& response, std::string& error)>;

// Роль (системный промпт) — статична для всего обхода. Формируется ОДИН раз
// (в worker) и переиспользуется для всех статей. Содержит инструкции/персону и
// заполненные статичные подстановки ({language}, {tone}, {max_words}); маркеры
// статьи ({title}/{body}) вырезаются — статья приходит в user-сообщении.
std::string build_role_prompt(const RewriteConfig& cfg);

// Пользовательское сообщение (контент одной статьи) — меняется на каждую
// новость. Содержит {title}→Заголовок: … и {body}→Текст: … .
std::string build_user_prompt(const Article& src, const RewriteConfig& cfg);

// Полный промпт (роль + контент) — для обратной совместимости и тестов.
// Равен склейке build_role_prompt + build_user_prompt.
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

// Полный рерайт: роль + контент статьи → llm → разбор ответа. Ошибки LLM не
// бросают исключений — возвращаются в RewriteResult. role_prompt — заранее
// собранная роль (см. build_role_prompt), чтобы не пересобирать идентичные
// инструкции для каждой новости.
RewriteResult rewrite_article(const Article& src, const RewriteConfig& cfg,
                              const std::string& role_prompt, const LlmFn& llm);

// Перегрузка: роль собирается из cfg внутри (для тестов/совместимости).
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

// Роль для SEO (системный промпт): статичные SEO-инструкции, маркеры статьи
// вырезаны. Формируется один раз на обход. language — целевой язык рерайта
// (заполняет {language} в шаблоне SEO, т.к. он вне RewriteConfig).
std::string build_seo_role_prompt(const SeoConfig& cfg, const std::string& language);

// Пользовательское сообщение для SEO: переписанная статья (заголовок/текст/язык).
std::string build_seo_user_prompt(const Article& src, const SeoConfig& cfg);

// Полный SEO-промпт (роль + контент) — для обратной совместимости/тестов.
std::string build_seo_prompt(const Article& src, const SeoConfig& cfg);

// Разбор ответа LLM: ищем первый '{' ... последний '}' и парсим JSON
// {focus_keyword, meta_description, seo_title} (устойчиво к markdown- fences).
SeoResult parse_seo_response(const std::string& response);

// Генерация SEO-данных: роль + контент статьи → llm → разбор. Best-effort.
SeoResult generate_seo(const Article& src, const SeoConfig& cfg,
                        const std::string& role_prompt, const LlmFn& llm);

// Перегрузка: роль собирается из cfg внутри (для тестов/совместимости).
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

// Роль для КОМБИНИРОВАННОГО рерайта+SEO (системный промпт): инструкции рерайта
// и СТРОГИЙ JSON-формат. Статична — формируется один раз на обход.
std::string build_combined_role_prompt(const RewriteConfig& cfg);

// Пользовательское сообщение для комбинированного рерайта+SEO: контент статьи.
std::string build_combined_user_prompt(const Article& src, const RewriteConfig& cfg);

// Полный комбинированный промпт (роль + контент) — для совместимости/тестов.
std::string build_combined_prompt(const Article& src, const RewriteConfig& cfg);

// Разбор комбинированного ответа LLM. Ищет первый '{' … последний '}' и
// парсит JSON. title+body обязательны для ok; отсутствие SEO-полей не портит
// рерайт (seo.ok=false — статью публикуем без SEO-мета).
RewriteSeoResult parse_rewrite_seo_response(const std::string& response);

// Рерайт + SEO ОДНИМ вызовом LLM (экономит облачные запросы/лимиты).
// При неудаче (сбой LLM/парсинга) ok=false; вызывающий может откатиться к
// двум отдельным вызовам. role_prompt — заранее собранная роль.
RewriteSeoResult rewrite_and_seo(const Article& src, const RewriteConfig& cfg,
                                  const std::string& role_prompt, const LlmFn& llm);

// Перегрузка: роль собирается из cfg внутри (для тестов/совместимости).
RewriteSeoResult rewrite_and_seo(const Article& src, const RewriteConfig& cfg,
                                  const LlmFn& llm);

} // namespace news_rewriter
