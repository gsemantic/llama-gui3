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
                                  "Формат ответа: первая строка — переписанный заголовок, "
                                  "затем пустая строка, затем переписанный текст новости.\n\n"
                                  "Заголовок: {title}\nТекст: {body}";
    int max_words = 0;               // 0 = без ограничения (примерный объём статьи)
};

struct SinkConfig {
    std::string type = "local_file";
    Json params = Json::object();
    std::string output_dir;          // пусто = каталог данных приложения
    std::string data_dir;            // каталог данных приложения (runtime, для .env)
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
    int max_items_per_source = 0;    // 0 = без ограничения (кол-во свежих статей)
    int max_age_hours = 0;           // 0 = без ограничения (свежесть в часах)
    int max_retries = 3;             // повторных попыток после 1-го сбоя источника
};

// Чистая сериализация/десериализация (без зависимостей от хоста).
Config default_config();
Json config_to_json(const Config& cfg);
Config config_from_json(const Json& j);

} // namespace news_rewriter
