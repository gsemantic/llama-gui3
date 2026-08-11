#include "fetcher.h"

#include <cctype>

#include "xml.h"

namespace news_rewriter {

namespace {

// Тело новости RSS: пробуем распространённые теги контента по порядку.
// Парсер сворачивает префиксы пространств имён (content:encoded → "encoded",
// yandex:full-text → "full-text"), поэтому имена сравниваются без префикса.
std::string rss_body(const XmlNode& item) {
    static const char* kBodyTags[] = {
        "description", "encoded", "full-text", "content", "summary",
    };
    for (const char* tag : kBodyTags) {
        const std::string v = child_text(item, tag);
        if (!v.empty()) return v;
    }
    return "";
}

// Извлечение RSS 2.0: <rss><channel><item>...
std::vector<FeedItem> extract_rss(const XmlNode& root) {
    std::vector<FeedItem> items;
    const XmlNode* channel = find_child(root, "channel");
    if (!channel) return items;
    for (const XmlNode* item : find_children(*channel, "item")) {
        FeedItem f;
        f.title = child_text(*item, "title");
        f.link = child_text(*item, "link");
        f.description = rss_body(*item);
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

// ---------------------------------------------------------------------------
// Автообнаружение ленты: если источником типа rss/atom указана HTML-страница,
// ищем в ней <link rel="alternate" type="application/rss+xml|atom+xml"> и
// возвращаем href этой ленты (или пустую строку, если ссылки нет).
// ---------------------------------------------------------------------------

// Значение атрибута в теге: "rel="alternate"" → "alternate" (без изменений регистра).
std::string attr_value(const std::string& tag, const std::string& name) {
    const std::string key = name + "=";
    std::size_t pos = 0;
    while ((pos = tag.find(key, pos)) != std::string::npos) {
        // Проверяем границу атрибута: перед "name=" не должно быть буквы/цифры/'-'.
        const bool boundary_ok =
            pos == 0 || !(std::isalnum(static_cast<unsigned char>(tag[pos - 1])) ||
                          tag[pos - 1] == '-');
        if (boundary_ok) {
            std::size_t v = pos + key.size();
            while (v < tag.size() && (tag[v] == ' ' || tag[v] == '\t')) v++;
            if (v < tag.size() && (tag[v] == '"' || tag[v] == '\'')) {
                const char quote = tag[v++];
                const std::size_t end = tag.find(quote, v);
                if (end != std::string::npos) {
                    return tag.substr(v, end - v);
                }
            }
        }
        pos += key.size();
    }
    return "";
}

// Сравнение строк в нижнем регистре (для type/rel — регистронезависимые).
bool equals_ci(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Абсолютный URL из (возможно относительного) href и базовой страницы.
std::string resolve_href(const std::string& href, const std::string& base) {
    if (href.empty() || base.empty()) return href;
    if (href.find("://") != std::string::npos) return href;       // уже абсолютный
    if (href.size() >= 2 && href[0] == '/' && href[1] == '/') {    // //host/path
        const std::size_t scheme = base.find("://");
        if (scheme == std::string::npos) return href;
        return base.substr(0, scheme + 3) + href.substr(2);
    }
    const std::size_t scheme = base.find("://");
    if (scheme == std::string::npos) return href;
    const std::size_t host_end = base.find('/', scheme + 3);
    const std::string origin = host_end == std::string::npos
                                   ? base : base.substr(0, host_end);
    if (!href.empty() && href[0] == '/') return origin + href;     // /path
    // Относительный путь: относительно каталога базовой страницы.
    const std::size_t last_slash = base.rfind('/');
    const std::string dir = last_slash != std::string::npos
                                ? base.substr(0, last_slash + 1) : origin + "/";
    return dir + href;
}

// Ищет в HTML первую ссылку на RSS/Atom ленту.
std::string discover_feed_link(const std::string& html, const std::string& base) {
    std::size_t pos = 0;
    while ((pos = html.find("<link", pos)) != std::string::npos) {
        const std::size_t gt = html.find('>', pos);
        if (gt == std::string::npos) break;
        const std::string tag = html.substr(pos, gt - pos + 1);
        const std::string type = attr_value(tag, "type");
        if (equals_ci(type, "application/rss+xml") || equals_ci(type, "application/atom+xml")) {
            const std::string rel = attr_value(tag, "rel");
            if (rel.empty() || equals_ci(rel, "alternate")) {
                const std::string href = attr_value(tag, "href");
                if (!href.empty()) return resolve_href(href, base);
            }
        }
        pos = gt + 1;
    }
    return "";
}

// Распространённое соглашение: лента лежит рядом со страницей по пути
// "<страница>/rss/" или "<страница>/rss" (Bitrix-порталы, WordPress и др.).
// Возвращает кандидата на проверку, или пустую строку.
std::string sibling_feed_candidate(const std::string& page_url) {
    std::string base = page_url;
    if (!base.empty() && base.back() == '/') base.pop_back();
    const std::size_t scheme = base.find("://");
    if (scheme == std::string::npos) return "";
    const std::size_t host_end = base.find('/', scheme + 3);
    if (host_end == std::string::npos || host_end + 1 >= base.size()) return "";
    return base + "/rss/";
}

} // namespace

bool Fetcher::init() {
    return http_.init();
}

bool Fetcher::is_available() const {
    return http_.is_available();
}

namespace {

// Корневые теги настоящих лент. Парсер XML «разбирает» и произвольный HTML
// (как дерево тегов), поэтому одного parse_xml недостаточно: проверяем корень.
bool is_feed_root(const XmlNode& root) {
    return root.name == "rss" || root.name == "feed" || root.name == "RDF";
}

// Разбор ленты (rss/atom) из тела ответа; возвращает true при успехе.
bool parse_feed_body(const std::string& body, std::vector<FeedItem>& out) {
    XmlNode root;
    if (!parse_xml(body, root)) return false;
    if (!is_feed_root(root)) return false;
    out = (root.name == "feed") ? extract_atom(root) : extract_rss(root);
    return true;
}

} // namespace

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
        if (parse_feed_body(resp.body, result.items)) {
            result.ok = true;
            return result;
        }

        // Тело не XML. Возможно, пользователь указал HTML-страницу, а не ленту.
        // 1) Ищем в странице ссылку на RSS/Atom.
        const std::string base = resp.final_url.empty() ? url : resp.final_url;
        const std::string feed_href = discover_feed_link(resp.body, base);

        // 2) Распространённое соглашение: лента по пути "<страница>/rss/".
        const std::string sibling = feed_href.empty() ? sibling_feed_candidate(base) : "";

        for (const std::string& candidate : {feed_href, sibling}) {
            if (candidate.empty()) continue;
            const HttpResponse feed_resp = http_.get(candidate, cfg);
            if (feed_resp.ok && feed_resp.status < 400 &&
                parse_feed_body(feed_resp.body, result.items)) {
                result.http_status = feed_resp.status;
                result.final_url = feed_resp.final_url;
                result.ok = true;
                return result;
            }
        }

        // Ленты нет: ошибка постоянная (не сеть/таймаут), ретраи бессмысленны.
        result.error = std::string("не удалось разобрать XML") +
                       (feed_href.empty() && sibling.empty()
                            ? " (в странице нет ссылки на RSS/Atom)"
                            : " (лента по найденной ссылке не разобрана)");
        result.permanent = true;
        return result;
    }

    result.error = "неизвестный тип источника: " + type;
    return result;
}

} // namespace news_rewriter
