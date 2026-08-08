#include "fetcher.h"

#include "xml.h"

namespace news_rewriter {

namespace {

// Извлечение RSS 2.0: <rss><channel><item>...
std::vector<FeedItem> extract_rss(const XmlNode& root) {
    std::vector<FeedItem> items;
    const XmlNode* channel = find_child(root, "channel");
    if (!channel) return items;
    for (const XmlNode* item : find_children(*channel, "item")) {
        FeedItem f;
        f.title = child_text(*item, "title");
        f.link = child_text(*item, "link");
        f.description = child_text(*item, "description");
        if (f.description.empty()) {
            f.description = child_text(*item, "encoded"); // content:encoded
        }
        f.pub_date = child_text(*item, "pubDate");
        if (!f.title.empty() || !f.link.empty()) {
            items.push_back(std::move(f));
        }
    }
    return items;
}

// Извлечение Atom: <feed><entry>...
std::vector<FeedItem> extract_atom(const XmlNode& root) {
    std::vector<FeedItem> items;
    for (const XmlNode* entry : find_children(root, "entry")) {
        FeedItem f;
        f.title = child_text(*entry, "title");
        f.description = child_text(*entry, "summary");
        if (f.description.empty()) f.description = child_text(*entry, "content");
        f.pub_date = child_text(*entry, "updated");
        if (f.pub_date.empty()) f.pub_date = child_text(*entry, "published");
        // <link href="..." rel="alternate">
        for (const XmlNode* link : find_children(*entry, "link")) {
            const std::string rel = link->attrs.count("rel") ? link->attrs.at("rel") : "alternate";
            if (rel == "alternate" || rel.empty()) {
                const std::string href = link->attrs.count("href") ? link->attrs.at("href") : "";
                if (!href.empty()) {
                    f.link = href;
                    break;
                }
            }
        }
        if (!f.title.empty() || !f.link.empty()) {
            items.push_back(std::move(f));
        }
    }
    return items;
}

} // namespace

bool Fetcher::init() {
    return http_.init();
}

bool Fetcher::is_available() const {
    return http_.is_available();
}

FetchResult Fetcher::fetch(const std::string& url, const std::string& type,
                           const NetworkConfig& cfg) {
    FetchResult result;

    const HttpResponse resp = http_.get(url, cfg);
    if (!resp.ok) {
        result.error = resp.error;
        return result;
    }

    result.http_status = resp.status;
    result.final_url = resp.final_url;

    if (resp.status >= 400) {
        result.error = "HTTP " + std::to_string(resp.status);
        return result;
    }

    if (type == "page") {
        result.ok = true;
        result.html = resp.body;
        return result;
    }

    if (type == "rss" || type == "atom") {
        XmlNode root;
        if (!parse_xml(resp.body, root)) {
            result.error = "не удалось разобрать XML";
            return result;
        }
        result.items = (root.name == "feed") ? extract_atom(root) : extract_rss(root);
        result.ok = true;
        return result;
    }

    result.error = "неизвестный тип источника: " + type;
    return result;
}

} // namespace news_rewriter
