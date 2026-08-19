#pragma once

#include <string>
#include <vector>

#include "config.h"
#include "http.h"

#include "headless_browser/headless_browser.h"

namespace news_rewriter {

// Элемент ленты (RSS/Atom).
struct FeedItem {
    std::string title;
    std::string link;
    std::string description;
    std::string pub_date;
    std::string image;   // URL заглавного изображения (media:content / enclosure / itunes:image / первый <img>)
};

// Результат загрузки источника.
struct FetchResult {
    bool ok = false;
    int http_status = 0;
    std::string error;
    std::string final_url;
    std::string html;                    // для type="page" (сырой HTML)
    std::vector<FeedItem> items;         // для rss/atom
    bool permanent = false;              // сбой не повторяемый (ретраи бессмысленны)
};

// Интерфейс загрузки (внедряется в Worker; тесты подставляют фейк).
class IFetch {
public:
    virtual ~IFetch() = default;
    virtual bool init() = 0;
    virtual bool is_available() const = 0;
    virtual FetchResult fetch(const std::string& url, const std::string& type,
                              const NetworkConfig& cfg) = 0;
};

// Реальный загрузчик: HTTP через libcurl (dlopen) + разбор RSS/Atom.
class Fetcher : public IFetch {
public:
    bool init() override;
    bool is_available() const override;
    FetchResult fetch(const std::string& url, const std::string& type,
                      const NetworkConfig& cfg) override;

private:
    HttpClient http_;
};

} // namespace news_rewriter
