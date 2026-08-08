#pragma once

#include <map>
#include <memory>
#include <string>

#include "common.h"
#include "config.h"
#include "storage.h"

namespace news_rewriter {

// Интерфейс «куда уходят готовые статьи» (ключ масштабируемости).
class Sink {
public:
    virtual ~Sink() = default;
    virtual bool write(const Article& article) = 0;
    virtual const char* name() const = 0;
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

} // namespace news_rewriter
