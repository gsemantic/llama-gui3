#include "fetcher.h"

#include <cctype>

#include "common.h"
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

// Заглавное изображение RSS-элемента: media:content (medium=image) →
// media:thumbnail → enclosure (type image) → itunes:image (href) → первый <img>.
bool equals_ci(const std::string& a, const std::string& b);  // см. ниже

std::string rss_image(const XmlNode& item, const std::string& description) {
    for (const auto& c : item.children) {
        if (c.name == "content" && c.attrs.count("url")) {
            const std::string medium =
                c.attrs.count("medium") ? c.attrs.at("medium") : "";
            if (medium.empty() || equals_ci(medium, "image")) return c.attrs.at("url");
        }
        if (c.name == "thumbnail" && c.attrs.count("url")) return c.attrs.at("url");
    }
    for (const auto& c : item.children) {
        if (c.name == "enclosure" && c.attrs.count("url")) {
            const std::string type = c.attrs.count("type") ? c.attrs.at("type") : "";
            if (type.empty() || type.find("image/") == 0) return c.attrs.at("url");
        }
        if (c.name == "image" && c.attrs.count("href")) return c.attrs.at("href");
    }
    // fallback: <img> внутри описания
    std::size_t pos = description.find("<img");
    if (pos != std::string::npos) {
        const std::size_t gt = description.find('>', pos);
        if (gt != std::string::npos) {
            const std::string tag = description.substr(pos, gt - pos + 1);
            const std::size_t q1 = tag.find("src=\"");
            if (q1 != std::string::npos) {
                const std::size_t e = tag.find('"', q1 + 5);
                if (e != std::string::npos) return tag.substr(q1 + 5, e - (q1 + 5));
            }
        }
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
        f.image = rss_image(*item, f.description);
        // Автор оригинала: dc:creator (парсер сворачивает префикс → "creator")
        // либо стандартный author.
        f.author = child_text(*item, "creator");
        if (f.author.empty()) f.author = child_text(*item, "author");
        if (!f.title.empty() || !f.link.empty()) {
            items.push_back(std::move(f));
        }
    }
    return items;
}

// Заглавное изображение Atom-элемента: media:content/thumbnail (url) →
// itunes:image (href) → <link rel="image|icon|thumbnail"> (href) → первый <img>.
std::string atom_image(const XmlNode& entry, const std::string& content) {
    for (const auto& c : entry.children) {
        if ((c.name == "content" || c.name == "thumbnail") && c.attrs.count("url"))
            return c.attrs.at("url");
        if (c.name == "image" && c.attrs.count("href")) return c.attrs.at("href");
    }
    for (const auto& c : entry.children) {
        if (c.name == "link") {
            const std::string rel = c.attrs.count("rel") ? c.attrs.at("rel") : "";
            if (rel == "image" || rel == "icon" || rel == "thumbnail") {
                const std::string href = c.attrs.count("href") ? c.attrs.at("href") : "";
                if (!href.empty()) return href;
            }
        }
    }
    std::size_t pos = content.find("<img");
    if (pos != std::string::npos) {
        const std::size_t gt = content.find('>', pos);
        if (gt != std::string::npos) {
            const std::string tag = content.substr(pos, gt - pos + 1);
            const std::size_t q1 = tag.find("src=\"");
            if (q1 != std::string::npos) {
                const std::size_t e = tag.find('"', q1 + 5);
                if (e != std::string::npos) return tag.substr(q1 + 5, e - (q1 + 5));
            }
        }
    }
    return "";
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
        // Atom: <author><name>Имя</name></author> → child_text берёт вложенный текст.
        f.author = child_text(*entry, "author");
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
        f.image = atom_image(*entry, f.description);
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

    // Перекодируем тело в UTF-8 (сайты вроде VK отдают windows-1251), иначе
    // кириллица ломает последующий разбор JSON и показывается «кракозябрами».
    const std::string body = to_utf8(resp.body, resp.content_type);

    if (type == "page") {
        std::string html = body;
        // Headless-рендер: принудительно (headless_enabled) либо авто-фолбэк,
        // когда обычный HTTP-фетч вернул пустую JS-оболочку (SPA, напр. VK.ru).
        // Рендеринг делегируется общей библиотеке headless_browser.
        headless_browser::RenderOptions opts;
        opts.browser_path = cfg.browser_path;
        opts.user_agent = cfg.user_agent;
        opts.timeout_ms = cfg.headless_timeout_ms;
        bool use_headless = cfg.headless_enabled;
        if (!use_headless && resp.ok && headless_browser::is_thin_content(html)) {
            use_headless = headless_browser::available(opts);
        }
        if (use_headless) {
            std::string err;
            const std::string dom = headless_browser::render_dom(url, opts, &err);
            if (!dom.empty()) {
                html = dom;  // Chromium сериализует DOM уже в UTF-8
            }
            // При неудаче оставляем обычный html (деградация без падения).
        }
        result.ok = true;
        result.html = html;
        return result;
    }

    if (type == "rss" || type == "atom") {
        if (parse_feed_body(body, result.items)) {
            result.ok = true;
            return result;
        }

        // Тело не XML. Возможно, пользователь указал HTML-страницу, а не ленту.
        // 1) Ищем в странице ссылку на RSS/Atom.
        const std::string base = resp.final_url.empty() ? url : resp.final_url;
        const std::string feed_href = discover_feed_link(body, base);

        // 2) Распространённое соглашение: лента по пути "<страница>/rss/".
        const std::string sibling = feed_href.empty() ? sibling_feed_candidate(base) : "";

        for (const std::string& candidate : {feed_href, sibling}) {
            if (candidate.empty()) continue;
            const HttpResponse feed_resp = http_.get(candidate, cfg);
            if (feed_resp.ok && feed_resp.status < 400) {
                const std::string feed_body =
                    to_utf8(feed_resp.body, feed_resp.content_type);
                if (parse_feed_body(feed_body, result.items)) {
                    result.http_status = feed_resp.status;
                    result.final_url = feed_resp.final_url;
                    result.ok = true;
                    return result;
                }
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
