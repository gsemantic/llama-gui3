#pragma once

#include <string>
#include <vector>

#include "json.h"

namespace news_rewriter {

// Ключ в настройках хоста, где хранится JSON-конфигурация плагина.
constexpr const char* kConfigKey = "news_rewriter.config";

// Маркеры извлечения для type="page" (пусто = эвристика).
struct SourceExtract {
    std::string title_marker;
    std::string body_marker;
};

struct SourceConfig {
    std::string url;
    std::string type;            // "rss" | "atom" | "page"
    SourceExtract extract;
    bool enabled = true;
};

struct RewriteConfig {
    std::string language = "ru";
    std::string tone = "нейтральный";
    std::string prompt_template = "Перепиши новость своими словами. "
                                  "Сохрани все факты. Язык: {language}. Тон: {tone}.\n\n"
                                  "Заголовок: {title}\nТекст: {body}";
};

struct SinkConfig {
    std::string type = "local_file";
    Json params = Json::object();
};

struct NetworkConfig {
    int timeout_seconds = 20;
    std::string user_agent = "news_rewriter/1.0";
    std::string proxy;           // пусто = системный прокси
    std::string extra_headers;   // "Header: value\n..." (для авторизации и пр.)
};

struct Config {
    std::vector<SourceConfig> sources;
    RewriteConfig rewrite;
    int schedule_minutes = 60;   // 0 = только ручной запуск
    SinkConfig sink;
    NetworkConfig network;
};

// Чистая сериализация/десериализация (без зависимостей от хоста).
Config default_config();
Json config_to_json(const Config& cfg);
Config config_from_json(const Json& j);

} // namespace news_rewriter
