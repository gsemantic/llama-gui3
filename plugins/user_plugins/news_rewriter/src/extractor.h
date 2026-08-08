#pragma once

#include <string>

#include "config.h"

namespace news_rewriter {

// Результат извлечения текста из HTML.
struct ExtractedArticle {
    std::string title;
    std::string body;
};

// HTML → чистый текст: снять теги, декодировать сущности, нормализовать
// пробелы/переносы, отсечь скрипты/стили и шумные строки.
std::string html_to_text(const std::string& html);

// HTML-описание из фида (description/summary) → чистый текст.
ExtractedArticle extract_from_description(const std::string& desc);

// HTML-страница → заголовок + основной текст.
// Если в cfg заданы маркеры — берётся текст между ними; иначе эвристика:
// заголовок из <h1>/<title>, тело — самый длинный связный набор «прозы» по
// плотности текста (исключая nav/header/footer/aside/form и заголовки h1..h6).
ExtractedArticle extract_page(const std::string& html, const SourceExtract& cfg);

} // namespace news_rewriter
