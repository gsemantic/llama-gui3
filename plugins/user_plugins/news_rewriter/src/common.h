#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

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
    std::string fetched_at;      // ISO-8601 UTC
    std::string title_original;
    std::string body_original;
    std::string title_rewritten;
    std::string body_rewritten;
    std::string language;
    TaskStatus status = TaskStatus::Pending;
    std::string error;
    uint32_t retry_count = 0;
    std::string content_hash;    // sha256(title + body)
};

// SHA-256 (hex) — для идентификации статей и дедупликации.
std::string sha256_hex(const std::string& data);

// Текущее время UTC в ISO-8601, напр. "2026-08-08T12:00:00Z".
std::string iso8601_now();

// Метка источника из URL (хост без схемы и порта).
std::string host_of(const std::string& url);

} // namespace news_rewriter
