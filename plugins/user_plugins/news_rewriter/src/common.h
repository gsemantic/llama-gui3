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
    std::string seo_focus_keyword;  // ключевое слово (модель, SEO-шаг)
    std::string seo_meta_description;// meta-описание (модель, SEO-шаг)
    std::string seo_title;          // SEO-заголовок (модель, SEO-шаг)
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

} // namespace news_rewriter
