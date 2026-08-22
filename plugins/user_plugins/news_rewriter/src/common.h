#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "json.h"

namespace news_rewriter {

// Общий колбэк логирования (main-поток перенаправляет в host log).
using LogFn = std::function<void(const std::string&)>;

// Состояние задачи/статьи в конвейере.
enum class TaskStatus {
    Pending,
    Fetching,
    Extracting,
    Rewriting,
    Exporting,
    Done,
    Error
};

const char* task_status_name(TaskStatus s);

// Готовая статья (модель данных, см. docs/ARCHITECTURE.md).
struct Article {
    std::string id;              // sha256(url)
    std::string url;
    std::string source;          // метка источника
    std::string fetched_at;      // ISO-8601 UTC (когда плагин забрал статью)
    std::int64_t published_at = 0;  // время публикации оригинала (Unix UTC), 0 = неизвестно
    std::string title_original;
    std::string body_original;
    std::string title_rewritten;
    std::string body_rewritten;
    std::string language;
    std::string source_image;       // URL заглавного изображения из источника (если есть)
    std::string author_original;     // автор оригинала (из ленты или страницы)
    std::string seo_focus_keyword;  // ключевое слово (модель, SEO-шаг)
    std::string seo_meta_description;// meta-описание (модель, SEO-шаг)
    std::string seo_title;          // SEO-заголовок (модель, SEO-шаг)
    std::string seo_slug;           // slug из focus_keyword (транслит, SEO-шаг)
    int seo_score = -1;             // итог SEO-скоркарда (0..100), -1 = не считалось
    std::string seo_issues_text;    // перечень POOR-метрик (человекочитаемо, из summary())
    TaskStatus status = TaskStatus::Pending;
    std::string error;
    uint32_t retry_count = 0;
    std::string content_hash;    // sha256(title + body)
};

// SHA-256 (hex) — для идентификации статей и дедупликации.
std::string sha256_hex(const std::string& data);

// Текущее время UTC в ISO-8601, напр. "2026-08-08T12:00:00Z".
std::string iso8601_now();

// То же для заданного момента (Unix UTC секунды). 0 → пустая строка.
std::string iso8601_of(std::int64_t sec);

// Разбор времени публикации ленты: RFC 822 ("Sat, 08 Aug 2026 11:51:47 +0300")
// или ISO 8601 ("2026-08-08T11:51:47Z"). Возвращает секунды с эпохи (UTC)
// или 0, если строка не распознана.
std::int64_t parse_feed_time(const std::string& s);

// Метка источника из URL (хост без схемы и порта).
std::string host_of(const std::string& url);

// Делает относительный/протокол-относительный URL абсолютным по базе страницы.
// Если url уже абсолютный (содержит "://") или base пуст — возвращает url как
// есть. Используется, чтобы заглавное изображение из источника (часто
// относительное) корректно сохранялось и публиковалось.
std::string resolve_url(const std::string& url, const std::string& base);

// Сериализация статьи в JSON (общая для Storage и Sink-ов).
Json article_to_json(const Article& a);

// Перекодировка тела HTTP-ответа в UTF-8 по декларируемой кодировке.
// content_type — значение заголовка Content-Type (напр.
// "text/html; charset=windows-1251"). Поддерживаются windows-1251/cp1251 и
// iso-8859-5 (наиболее распространённые для русскоязычных сайтов, напр. VK).
// Для UTF-8/пустого заголовка текст возвращается как есть. Без этой
// перекодировки CP1251-кириллица (байты 0x80–0xFF) ломает последующий разбор
// JSON (облако отдаёт invalid UTF-8) и отображается «кракозябрами».
std::string to_utf8(const std::string& text, const std::string& content_type);

// Гарантирует валидный UTF-8: если строка уже корректный UTF-8 — возвращает
// как есть, иначе перекодирует из windows-1251. Используется как страховка на
// границе вывода, чтобы невалидные байты (отображаемые ImGui как «?») не
// попадали в интерфейс ни по какому пути.
std::string ensure_utf8(const std::string& text);

// Содержит ли строка хотя бы один кириллический символ (блок U+0400–U+04FF).
// Используется, чтобы не дублировать подпись «Автор оригинала» для имён,
// уже записанных кириллицей.
bool has_cyrillic(const std::string& text);

} // namespace news_rewriter
