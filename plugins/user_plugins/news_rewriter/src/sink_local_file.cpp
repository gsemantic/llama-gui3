#include "sink.h"

#include <filesystem>

namespace news_rewriter {

namespace fs = std::filesystem;

namespace {

// v1: запись статей на диск через Storage (.json + .md).
class LocalFileSink : public Sink {
public:
    LocalFileSink(Storage& storage, const LogFn& log)
        : storage_(storage), log_(log) {}

    bool write(const Article& article) override {
        if (!storage_.ready()) {
            if (log_) log_("LocalFileSink: хранилище не инициализировано");
            return false;
        }
        const bool j = storage_.save_article_json(article);
        const bool m = storage_.save_article_md(article);
        if (!j || !m) {
            if (log_) log_("LocalFileSink: ошибка записи статьи " + article.url);
            return false;
        }
        if (log_) log_("LocalFileSink: сохранено " + article.url);
        return true;
    }

    // Статья уже записана, если существует её .json-файл. Если файл удалили
    // вручную, дедуп это узнает и переиздаст статью.
    Presence presence(const Article& a) const override {
        if (!storage_.ready()) return Presence::Unknown;
        return fs::exists(storage_.article_json_path(a)) ? Presence::Present
                                                         : Presence::Absent;
    }

    const char* name() const override { return "local_file"; }

private:
    Storage& storage_;
    LogFn log_;
};

} // namespace

// Регистрируется в ll_plugin_init (см. plugin_main.cpp).
std::unique_ptr<Sink> make_local_file_sink(const SinkConfig&, Storage& storage,
                                           const LogFn& log) {
    return std::make_unique<LocalFileSink>(storage, log);
}

} // namespace news_rewriter
