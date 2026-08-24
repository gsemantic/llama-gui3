#pragma once

#include <string>

#include "common.h"
#include "config.h"

namespace news_rewriter {

// Результат извлечения текста из HTML.
struct ExtractedArticle {
    std::string title;
    std::string body;
    std::string image;   // URL заглавного изображения (og:image / twitter:image / первый <img>)
    std::string author;  // автор оригинала (мета author / подпись «作者：…» / «Автор: …»)
    std::string url;     // ссылка на статью (для страниц-списков: заполняется краулером)
    std::int64_t published_at = 0;  // время публикации (Unix UTC), 0 = неизвестно
};

// HTML → чистый текст: снять теги, декодировать сущности, нормализовать
// пробелы/переносы, отсечь скрипты/стили и шумные строки.
std::string html_to_text(const std::string& html);

// HTML-описание из фида (description/summary) → чистый текст.
ExtractedArticle extract_from_description(const std::string& desc);

// Извлекает ВСЕ ВНЕШНИЕ ссылки страницы (href + видимый текст якоря), резолвя
// относительные URL по base_url. «Внешняя» = хост ссылки отличается от хоста
// base_url (т.е. не внутренняя для сайта-источника). Отсекаются служебные
// схемы (javascript:/mailto:/tel:/#), пустые и дубликаты. Используется, чтобы
// сохранить ссылки оригинала в выходной статье (links.preserve_external).
std::vector<ExternalLink> extract_external_links(const std::string& html,
                                                 const std::string& base_url);

// HTML-страница → заголовок + основной текст.
// Если в cfg заданы маркеры — берётся текст между ними; иначе эвристика:
// заголовок из <h1>/<title>, тело — самый длинный связный набор «прозы» по
// плотности текста (исключая nav/header/footer/aside/form и заголовки h1..h6).
// base_url нужен, чтобы резолвить относительные ссылки на фото в абсолютные.
ExtractedArticle extract_page(const std::string& html, const std::string& base_url,
                              const SourceExtract& cfg);

// Первое «содержательное» изображение статьи: пропускает логотипы, иконки,
// превью для соцсетей (og/social/preview/teaser) и прочий декор, оставляя
// чистое фото из тела материала. Используется extract_page вместо og:image,
// который часто содержит наложение заголовка/логотипа сайта. Поиск ограничен
// диапазоном [from, to) (по умолчанию — весь html), что позволяет искать
// картинку только внутри тела статьи, игнорируя декор шапки/подвала.
std::string first_content_image(const std::string& html,
                                std::size_t from = 0,
                                std::size_t to = std::string::npos);

// HTML-страница (в т.ч. список/категория) → один или несколько блоков статей.
// Если страница — список (несколько <article> / повторяющихся ссылок на
// новости), возвращает по одному ExtractedArticle на каждую найденную статью
// (title/url/image/короткий сниппет). Иначе — один элемент (вся страница как
// одна статья, поведение extract_page). base_url нужен для резолва относительных
// ссылок. Вызывающий (worker) для каждого элемента с непустым url подгружает
// реальную страницу статьи и извлекает полный текст.
std::vector<ExtractedArticle> extract_page_items(const std::string& html,
                                                 const std::string& base_url,
                                                 const SourceExtract& cfg);

// Разбор ВХОДНОГО МАТЕРИАЛА в виде ПРОСТОГО ТЕКСТА (plaintext, без HTML):
// извлекает имя автора оригинала, если оно указано в ОТДЕЛЬНОМ абзаце
// (собственная строка/абзац), и возвращает очищенное тело без этой подписи.
//
// Поддерживаемые формы подписи в отдельном абзаце:
//   - с явным маркером (в той же строке): «Автор: Имя», «Автор статьи: Имя»,
//     «Автор оригинала: Имя», «By Имя», «Author: Имя», «作者：Имя»;
//   - маркер без имени (строка «Автор:» / «By») — имя берётся из СЛЕДУЮЩЕГО
//     абзаца;
//   - без маркера: короткий абзац из 1–3 слов, каждое с заглавной буквы
//     (ФИО/инициалы), стоящий последним в материале.
//
// Возвращает true, если автор найден; author — имя, body — текст без подписи.
// Если не найдено — возвращает false, body == исходному text.
bool extract_author_from_text(const std::string& text, std::string& author,
                              std::string& body);

// Один кандидат извлечения для режима разведки (предпросмотра). Отличается от
// ExtractedArticle тем, что несёт идентификатор стратегии и оценку качества,
// чтобы UI мог предложить пользователю несколько вариантов «текст + фото» и
// запомнить, какой из них был одобрен.
struct ExtractionProposal {
    ExtractedArticle article;     // title/body/image как в обычном извлечении
    int strategy = 0;             // id стратегии (0..N)
    std::string strategy_name;    // человекочитаемое имя (для лога/UI)
    double score = 0.0;          // оценка «объёма» текста (для сортировки)
};

// Несколько вариантов извлечения одной страницы (type="page") для режима
// предпросмотра. Каждый вариант — самостоятельная стратегия (разные источники
// заголовка/обложки), что даёт пользователю реальный выбор при «разведке»
// незнакомой вёрстки. Маркеры в cfg (body_marker/title_marker) дают ровно один
// детерминированный вариант. base_url нужен для резолва относительных ссылок.
std::vector<ExtractionProposal> extract_page_candidates(
    const std::string& html,
    const std::string& base_url,
    const SourceExtract& cfg);

} // namespace news_rewriter
