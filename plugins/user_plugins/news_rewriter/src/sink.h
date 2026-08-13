#pragma once

#include <map>
#include <memory>
#include <string>

#include "common.h"
#include "config.h"
#include "storage.h"

namespace news_rewriter {

// Состояние статьи в приёмнике (для сверки дедупа с реальностью).
enum class Presence {
    Unknown,   // не удалось определить (нет связи, нет прав, опция выключена)
    Present,   // статья УЖЕ есть в приёмнике (пропускаем, не дублируем)
    Absent     // в приёмнике НЕТ (локальная метка дедупа устарела — переиздаём)
};

// Интерфейс «куда уходят готовые статьи» (ключ масштабируемости).
class Sink {
public:
    virtual ~Sink() = default;
    virtual bool write(const Article& article) = 0;
    virtual const char* name() const = 0;

    // Реальное состояние статьи в приёмнике (а не в локальном index.json).
    // Позволяет дедупу сверяться с сайтом/хранилищем: если черновик удалён
    // на сайте, плагин узнает об этом и переиздаст статью. По умолчанию
    // Unknown — плагин полагается только на локальный индекс.
    virtual Presence presence(const Article&) const { return Presence::Unknown; }
};

// Фабрика sink-а по конфигурации. Реализация сама решает, как применить
// cfg.params и куда писать (через Storage).
using SinkFactory = std::unique_ptr<Sink>(*)(const SinkConfig& cfg,
                                             Storage& storage, const LogFn& log);

// Реестр sink-ов: регистрация по типу из конфига (этап 6 добавляет новые
// модули без правок ядра).
class SinkRegistry {
public:
    static SinkRegistry& instance();

    void register_factory(const char* type, SinkFactory factory);
    std::unique_ptr<Sink> create(const SinkConfig& cfg, Storage& storage,
                                 const LogFn& log);

private:
    SinkRegistry() = default;
    std::map<std::string, SinkFactory> factories_;
};

// Фабрика LocalFileSink (v1). Регистрируется в ll_plugin_init под "local_file".
std::unique_ptr<Sink> make_local_file_sink(const SinkConfig& cfg, Storage& storage,
                                           const LogFn& log);

// Фабрика HttpSink (этап 6). Регистрируется под "http"; отправляет статью
// JSON-POST'ом на cfg.params.url (параметры: url, api_key, timeout_seconds).
std::unique_ptr<Sink> make_http_sink(const SinkConfig& cfg, Storage& storage,
                                      const LogFn& log);

// Фабрика WordPressSink (этап 7). Регистрируется под "wordpress"; публикует
// статью в WP через REST API (/wp-json/wp/v2/posts) с Basic-авторизацией
// (Application Password). Параметры: site_url, username, app_password, status,
// categories, tags, author, excerpt, slug, featured_image, timeout_seconds,
// max_retries, retry_delay_ms.
std::unique_ptr<Sink> make_wordpress_sink(const SinkConfig& cfg, Storage& storage,
                                          const LogFn& log);

// Проверка связи с WordPress: аутентифицируется через
// GET {site_url}/wp-json/wp/v2/users/me (Application Password, Basic).
// Возвращает человекочитаемый статус (начинается с "OK" при успехе).
std::string wordpress_check_connection(const std::string& site_url,
                                       const std::string& user,
                                       const std::string& app_password);

} // namespace news_rewriter
