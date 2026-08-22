#include "sink.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "dotenv.h"
#include "http.h"
#include "json.h"

namespace news_rewriter {

namespace {

// --- helpers ---------------------------------------------------------------

std::string rtrim(const std::string& s, char ch) {
    std::string out = s;
    while (!out.empty() && out.back() == ch) out.pop_back();
    return out;
}

// Экранирование спецсимволов для вставки текста в HTML.
std::string html_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c; break;
        }
    }
    return out;
}

// Простейший Markdown → HTML внутри одного абзаца (до разбиения на <p>).
std::string inline_markdown(const std::string& text) {
    std::string out = html_escape(text);
    // **жирный** → <strong>…</strong>
    std::string bold;
    bool in_bold = false;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '*' && i + 1 < out.size() && out[i + 1] == '*') {
            bold += in_bold ? "</strong>" : "<strong>";
            in_bold = !in_bold;
            ++i;
            continue;
        }
        bold += out[i];
    }
    out = bold;
    // *курсив* → <em>…</em> (одиночная звёздочка, не внутри <strong>)
    std::string em;
    bool in_em = false;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '*') {
            em += in_em ? "</em>" : "<em>";
            in_em = !in_em;
            continue;
        }
        em += out[i];
    }
    return em;
}

// Превращает body_rewritten (текст/лёгкий Markdown) в HTML для WP:
//   - блоки по \n\n → <p>…</p>
//   - одинарные \n внутри блока → <br>
//   - # заголовок → <h2>
std::string body_to_html(const std::string& body) {
    if (body.empty()) return "";
    std::vector<std::string> blocks;
    std::string cur;
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '\n' && i + 1 < body.size() && body[i + 1] == '\n') {
            blocks.push_back(cur);
            cur.clear();
            ++i;
            continue;
        }
        cur += body[i];
    }
    if (!cur.empty()) blocks.push_back(cur);

    std::string html;
    for (auto block : blocks) {
        // Обрезка краевых пробелов/переносов.
        const std::size_t b = block.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        const std::size_t e = block.find_last_not_of(" \t\r\n");
        block = block.substr(b, e - b + 1);

        if (block.rfind("# ", 0) == 0) {
            html += "<h2>" + inline_markdown(block.substr(2)) + "</h2>\n";
        } else {
            std::string with_br;
            std::string line;
            for (std::size_t i = 0; i < block.size(); ++i) {
                if (block[i] == '\n') {
                    with_br += inline_markdown(line) + "<br>\n";
                    line.clear();
                } else {
                    line += block[i];
                }
            }
            if (!line.empty()) with_br += inline_markdown(line);
            html += "<p>" + with_br + "</p>\n";
        }
    }
    return html;
}

// Base64 (RFC 4648) для HTTP Basic авторизации.
std::string base64_encode(const std::string& in) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 2 < in.size()) {
        const uint32_t n = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) |
                           uint8_t(in[i + 2]);
        out += tbl[(n >> 18) & 0x3F];
        out += tbl[(n >> 12) & 0x3F];
        out += tbl[(n >> 6) & 0x3F];
        out += tbl[n & 0x3F];
        i += 3;
    }
    const std::size_t rem = in.size() - i;
    if (rem == 1) {
        const uint32_t n = uint8_t(in[i]) << 16;
        out += tbl[(n >> 18) & 0x3F];
        out += tbl[(n >> 12) & 0x3F];
        out += "==";
    } else if (rem == 2) {
        const uint32_t n = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8);
        out += tbl[(n >> 18) & 0x3F];
        out += tbl[(n >> 12) & 0x3F];
        out += tbl[(n >> 6) & 0x3F];
        out += "=";
    }
    return out;
}

std::string mime_from_url(const std::string& url) {
    const std::size_t q = url.find('?');
    const std::string path = (q == std::string::npos) ? url : url.substr(0, q);
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "application/octet-stream";
    const std::string ext = path.substr(dot + 1);
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "svg") return "image/svg+xml";
    return "application/octet-stream";
}

std::string filename_from_url(const std::string& url) {
    const std::size_t q = url.find('?');
    const std::string path = (q == std::string::npos) ? url : url.substr(0, q);
    const std::size_t slash = path.find_last_of('/');
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    if (name.empty()) name = "image";
    return name;
}

// Распознаёт тип картинки по «магическим» байтам (сигнатурам формата), чтобы
// WordPress получал корректный Content-Type и расширение даже для CDN-URL БЕЗ
// расширения. Напр. Дзен/Яндекс отдают картинки по opaque-ссылкам
// (avatars.mds.yandex.net/...), и mime_from_url() выдаёт
// application/octet-stream — WP REST /media такое отвергает с HTTP 500, из-за
// чего картинка не попадает в медиатеку и остаётся ссылкой на исходник.
std::string detect_image_mime(const std::string& data) {
    const std::size_t n = data.size();
    if (n >= 3 &&
        static_cast<unsigned char>(data[0]) == 0xFF &&
        static_cast<unsigned char>(data[1]) == 0xD8 &&
        static_cast<unsigned char>(data[2]) == 0xFF) {
        return "image/jpeg";
    }
    if (n >= 8 && data[0] == '\x89' && data[1] == 'P' && data[2] == 'N' &&
        data[3] == 'G' && data[4] == '\r' && data[5] == '\n' &&
        data[6] == '\x1A' && data[7] == '\n') {
        return "image/png";
    }
    if (n >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F' &&
        data[3] == '8' && (data[4] == '7' || data[4] == '9') && data[5] == 'a') {
        return "image/gif";
    }
    if (n >= 12 && data.compare(0, 4, "RIFF") == 0 &&
        data.compare(8, 4, "WEBP") == 0) {
        return "image/webp";
    }
    if (n >= 2 && data[0] == 'B' && data[1] == 'M') return "image/bmp";
    return "";
}

std::string ext_for_mime(const std::string& mime) {
    if (mime == "image/jpeg") return "jpg";
    if (mime == "image/png") return "png";
    if (mime == "image/gif") return "gif";
    if (mime == "image/webp") return "webp";
    if (mime == "image/bmp") return "bmp";
    return "";
}

// --- WordPressSink ----------------------------------------------------------

class WordPressSink : public Sink {
public:
    WordPressSink(const SinkConfig& cfg, Storage& storage, const LogFn& log)
        : env_path_(!cfg.data_dir.empty()
                       ? cfg.data_dir + "/news_rewriter/.env"
                       : (storage.root().empty() ? std::string(".env")
                                                 : storage.root() + "/.env")),
          storage_(storage),
          verify_site_state_(
              cfg.params.get("verify_site_state").as_bool(true)),
          site_url_(rtrim(cfg.params.get("site_url").as_string(), '/')),
          username_(cfg.params.get("username").as_string()),
          app_password_(cfg.params.get("app_password").as_string()),
          status_(cfg.params.get("status").as_string("draft")),
          post_type_(cfg.params.get("post_type").as_string("posts")),
          excerpt_(cfg.params.get("excerpt").as_string()),
          slug_(cfg.params.get("slug").as_string()),
          featured_image_(cfg.params.get("featured_image").as_string()),
          timeout_(static_cast<int>(
              cfg.params.get("timeout_seconds").as_int(20))),
          max_retries_(static_cast<int>(
              cfg.params.get("max_retries").as_int(0))),
          retry_delay_ms_(static_cast<int>(
              cfg.params.get("retry_delay_ms").as_int(1000))),
          log_(log) {
        // Опциональные числовые id-массивы/скаляры.
        const Json& cats = cfg.params.get("categories");
        if (cats.is_array()) {
            for (std::size_t i = 0; i < cats.size(); ++i)
                categories_.push_back(static_cast<int>(cats[i].as_int(0)));
        }
        const Json& tags = cfg.params.get("tags");
        if (tags.is_array()) {
            for (std::size_t i = 0; i < tags.size(); ++i)
                tags_.push_back(static_cast<int>(tags[i].as_int(0)));
        }
        if (cfg.params.get("author").is_number())
            author_ = static_cast<int>(cfg.params.get("author").as_int(0));
        // Нормализация app_password: убираем пробелы перед кодированием.
        std::string norm;
        for (char c : app_password_) if (c != ' ') norm += c;
        app_password_ = norm;
    }

    bool write(const Article& article) override {
        // Учётные данные: из .env (приоритет), иначе из params (fallback/тесты).
        std::string user = dotenv_read(env_path_, kNewsRewriterWpUser);
        if (user.empty()) user = username_;
        std::string pass = dotenv_read(env_path_, kNewsRewriterWpPass);
        if (pass.empty()) pass = app_password_;
        {
            std::string norm;
            for (char c : pass) if (c != ' ') norm += c;
            pass = norm;
        }
        if (site_url_.empty() || user.empty() || pass.empty()) {
            if (log_) {
                log_("WordPressSink: не заданы site_url/логин/пароль "
                     "(проверьте " + env_path_ + " и параметры sink)");
            }
            return false;
        }
        if (article.title_rewritten.empty() || article.body_rewritten.empty()) {
            if (log_) log_("WordPressSink: пустой рерайт (title/body), пропуск");
            return false;
        }
        if (!client_.init()) {
            if (log_) log_("WordPressSink: libcurl недоступен");
            return false;
        }

        NetworkConfig nc;
        nc.timeout_seconds = timeout_;
        const std::string endpoint = site_url_ + "/wp-json/wp/v2/" + post_type_;
        const std::string auth = base64_encode(user + ":" + pass);
        // Заглавное изображение: ручной URL из параметров sink либо авто —
        // картинка из источника (Article.source_image). Резолвим в абсолютный,
        // т.к. в источнике оно часто относительное.
        std::string image_url =
            featured_image_.empty()
                ? resolve_url(article.source_image, article.url)
                : featured_image_;

        // Заливаем картинку в медиабиблиотеку WP ДО создания поста, чтобы и
        // обложка, и встроенное в текст фото ссылались на картинку с хостинга
        // WP, а не на исходник источника. Если заливка не удалась — откатываемся
        // на прямую ссылку на исходник (чтобы пост всё равно нес фото).
        const std::string alt = article.seo_focus_keyword.empty()
                                    ? article.title_rewritten
                                    : article.seo_focus_keyword;
        int media_id = 0;
        std::string media_url;
        if (!image_url.empty()) {
            const auto m = upload_media(image_url, nc, user, pass, alt,
                                        article.url);
            media_id = m.first;
            media_url = m.second;
        }

        // Тело статьи в HTML. Hero-картинку вставляем в текст ТОЛЬКО если не
        // удалось залить её в медиатеку WP (media_id == 0): тогда обложка
        // (featured_media) не задана и пост бы остался без фото. Если заливка
        // прошла — тема уже показывает изображение через featured_media, и
        // дублировать его внутри текста (в оригинальном размере) не нужно.
        std::string html = body_to_html(article.body_rewritten);
        if (media_id == 0) {
            const std::string hero_src = media_url.empty() ? image_url : media_url;
            if (!hero_src.empty()) {
                html = "<p><img src=\"" + html_escape(hero_src) + "\" alt=\"" +
                       html_escape(alt) + "\"></p>\n" + html;
            }
        }
        {
            const std::string host = host_of(article.url);
            html += "\n<p>Источник: <a href=\"" + html_escape(article.url) +
                    "\">" + html_escape(host) + "</a></p>";
        }
        // Примечание: подпись «Автор оригинала» теперь формируется самим LLM в
        // теле рерайта (с кириллической транслитерацией имени в скобках), поэтому
        // здесь её не дублируем — иначе будет две разные строки автора.
        if (article.published_at > 0) {
            html += "<p>Дата оригинала: " +
                    html_escape(iso8601_of(article.published_at)) + "</p>";
        }

        Json body = Json::object();
        body["title"] = article.title_rewritten;
        body["content"] = html;
        body["status"] = status_;
        if (!excerpt_.empty()) body["excerpt"] = excerpt_;
        // Стабильный slug от источника (если не задан вручную в params),
        // чтобы дедуп по сайту мог надёжно находить ранее созданный пост.
        // Стабильный slug от источника (если не задан вручную в params),
        // либо оптимизированный SEO-slug из focus_keyword, когда доступен.
        if (!slug_.empty()) {
            body["slug"] = slug_;
        } else if (!article.seo_slug.empty()) {
            body["slug"] = article.seo_slug;
        } else {
            body["slug"] = storage_.slug_for(article.url);
        }
        // Обложка поста — id картинки, залитой в медиатеку WP выше.
        if (media_id != 0) body["featured_media"] = static_cast<int64_t>(media_id);
        // Категории/теги поддерживают не все типы: стандартные «страницы»
        // (pages) их не принимают — WP вернёт 400. Для них не шлём таксономию.
        if (post_type_ != "pages") {
            if (!categories_.empty()) {
                Json arr = Json::array();
                for (int id : categories_) arr.push(static_cast<int64_t>(id));
                body["categories"] = arr;
            }
            if (!tags_.empty()) {
                Json arr = Json::array();
                for (int id : tags_) arr.push(static_cast<int64_t>(id));
                body["tags"] = arr;
            }
        }
        if (author_ != 0) body["author"] = static_cast<int64_t>(author_);

        // Авто-SEO: прокидываем наши собственные мета-ключи nr_seo_*. Читаются
        // тонким MU-плагином nr-seo.php (см. mu-plugins/nr-seo.php) и выводятся
        // в <head> — замена тяжёлым SEO-плагинам вроде Yoast/RankMath. Поля
        // заполняются рерайтером только если в конфиге включен seo.enabled.
        {
            Json meta = Json::object();
            if (!article.seo_focus_keyword.empty())
                meta["nr_seo_keyword"] = article.seo_focus_keyword;
            if (!article.seo_meta_description.empty())
                meta["nr_seo_description"] = article.seo_meta_description;
            if (!article.seo_title.empty())
                meta["nr_seo_title"] = article.seo_title;
            if (!meta.empty()) body["meta"] = meta;
        }

        // Excerpt из SEO-meta, если не задан вручную в параметрах sink.
        if (excerpt_.empty() && !article.seo_meta_description.empty()) {
            body["excerpt"] = article.seo_meta_description;
        }

        std::vector<std::string> headers = {
            "Content-Type: application/json",
            "Authorization: Basic " + auth};

        const std::string payload = body.dump();
        int attempt = 0;
        HttpResponse resp;
        for (;;) {
            resp = client_.post(endpoint, payload, nc, headers);
            if (resp.ok && resp.status == 201) break;
            if (log_) {
                log_("WordPressSink: создание поста не удалось (HTTP " +
                     std::to_string(resp.status) + "): " +
                     (resp.error.empty() ? resp.body : resp.error));
            }
            if (attempt >= max_retries_) return false;
            if (retry_delay_ms_ > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(retry_delay_ms_));
            }
            attempt++;
        }

        const int post_id = parse_post_id(resp.body);
        if (log_) {
            log_("WordPressSink: объект «" + post_type_ + "» создан id=" +
                 std::to_string(post_id) + " (HTTP 201) для " + article.url);
        }

        // Обложка (featured_media) уже залита в медиатеку WP выше и привязана к
        // посту через поле featured_media в теле запроса. Повторная заливка не
        // нужна.
        return true;
    }

    const char* name() const override { return "wordpress"; }

    // Реальное состояние статьи на сайте: есть ли уже пост с нашим slug-ом.
    // Возвращает Present/Absent (при успешном запросе) либо Unknown, если
    // сверка отключена или сайт недоступен/нет прав.
    Presence presence(const Article& a) const override {
        if (!verify_site_state_) return Presence::Unknown;
        std::string user, pass;
        if (!resolve_credentials(user, pass)) return Presence::Unknown;

        HttpClient client;
        if (!client.init()) return Presence::Unknown;
        const std::string slug = storage_.slug_for(a.url);
        const std::string ep = site_url_ + "/wp-json/wp/v2/" + post_type_ +
                               "?slug=" + slug + "&status=any&per_page=1";
        NetworkConfig nc;
        nc.timeout_seconds = timeout_;
        const std::string auth = "Authorization: Basic " +
                                 base64_encode(user + ":" + pass);
        HttpResponse resp = client.get(ep, nc, std::vector<std::string>{auth});
        if (!resp.ok || resp.status != 200) {
            if (log_) {
                log_("WordPressSink: не удалось сверить состояние сайта "
                     "(HTTP " + std::to_string(resp.status) + "), дедуп по "
                     "локальному индексу");
            }
            return Presence::Unknown;
        }
        bool ok = false;
        Json j = Json::parse(resp.body, &ok);
        if (!ok || !j.is_array()) return Presence::Unknown;
        return j.size() > 0 ? Presence::Present : Presence::Absent;
    }

private:
    // Считывает учётные данные (.env приоритет, иначе params) и нормализует
    // app_password. Возвращает false, если заданы не все данные.
    bool resolve_credentials(std::string& user, std::string& pass) const {
        user = dotenv_read(env_path_, kNewsRewriterWpUser);
        if (user.empty()) user = username_;
        pass = dotenv_read(env_path_, kNewsRewriterWpPass);
        if (pass.empty()) pass = app_password_;
        auto norm = [](std::string s) {
            std::string out;
            for (char c : s) if (c != ' ') out += c;
            return out;
        };
        pass = norm(pass);
        return !site_url_.empty() && !user.empty() && !pass.empty();
    }

    static int parse_post_id(const std::string& body) {        bool ok = false;
        Json j = Json::parse(body, &ok);
        if (!ok) return 0;
        const Json& id = j["id"];
        if (id.is_number()) return static_cast<int>(id.as_int(0));
        return 0;
    }

    // Загружает картинку по URL в медиабиблиотеку WP и возвращает
    // (media_id, source_url). source_url — это уже адрес картинки на хостинге
    // WP (а не исходник источника), его и нужно вставлять в текст поста и
    // использовать как обложку. alt — alt-текст обложки (обычно ключевое слово
    // модели из SEO-шага).
    std::pair<int, std::string> upload_media(const std::string& image_url,
                                              const NetworkConfig& nc,
                                              const std::string& user,
                                              const std::string& pass,
                                              const std::string& alt = "",
                                              const std::string& referer = "") {
        std::pair<int, std::string> none{0, ""};
        // CDN (Дзен/Яндекс) нередко требуют Referer, иначе отдают заглушку
        // вместо картинки — заливка такого «файла» в WP падает с 500.
        std::vector<std::string> dl_headers;
        if (!referer.empty()) dl_headers.push_back("Referer: " + referer);
        HttpResponse img = client_.get(image_url, nc, dl_headers);
        if (!img.ok || img.body.empty()) {
            if (log_) log_("WordPressSink: не удалось скачать картинку " + image_url);
            return none;
        }
        // Тип берём по РЕАЛЬНЫМ байтам файла, а не по расширению в URL (CDN
        // часто без расширения → application/octet-stream → WP отвергает → 500).
        std::string mime = detect_image_mime(img.body);
        if (mime.empty()) mime = mime_from_url(image_url);
        std::string fname = filename_from_url(image_url);
        const std::string ext = ext_for_mime(mime);
        if (!ext.empty()) {
            const std::size_t dot = fname.find_last_of('.');
            if (dot == std::string::npos) fname += "." + ext;
            else fname = fname.substr(0, dot + 1) + ext;
        }
        const std::string media_ep = site_url_ + "/wp-json/wp/v2/media";
        const std::string auth = base64_encode(user + ":" + pass);

        std::vector<std::string> headers = {
            "Content-Type: " + mime,
            "Content-Disposition: attachment; filename=\"" + fname + "\"",
            "Authorization: Basic " + auth};

        HttpResponse media_resp = client_.post(media_ep, img.body, nc, headers);
        if (!media_resp.ok || media_resp.status != 201) {
            if (log_) {
                std::string detail = media_resp.body;
                if (detail.size() > 300) detail = detail.substr(0, 300) + "…";
                log_("WordPressSink: загрузка медиа не удалась (HTTP " +
                     std::to_string(media_resp.status) + ")" +
                     (detail.empty() ? std::string("") : ": " + detail));
            }
            return none;
        }
        const int media_id = parse_post_id(media_resp.body);
        if (media_id == 0) return none;

        // Адрес картинки на хостинге WP (именно его подставляем в текст/обложку).
        bool ok = false;
        Json mj = Json::parse(media_resp.body, &ok);
        std::string source_url = ok ? mj.get("source_url").as_string() : std::string();

        // alt-текст обложки (ключевое слово модели из SEO-шага).
        if (!alt.empty() && !source_url.empty()) {
            Json altpatch = Json::object();
            altpatch["alt_text"] = alt;
            std::vector<std::string> ah = {
                "Content-Type: application/json",
                "Authorization: Basic " + auth};
            client_.post(media_ep + "/" + std::to_string(media_id),
                         altpatch.dump(), nc, ah);
        }
        if (log_) {
            log_("WordPressSink: медиа загружено media_id=" +
                 std::to_string(media_id) + " (" + source_url + ") для " +
                 image_url);
        }
        return {media_id, source_url};
    }

    HttpClient client_;
    std::string env_path_;      // <data_dir>/news_rewriter/.env (секреты)
    Storage& storage_;         // для стабильного slug-а (сверка с сайтом)
    bool verify_site_state_ = true;  // сверять дедуп с реальным состоянием сайта
    std::string site_url_;
    std::string username_;
    std::string app_password_;
    std::string status_;
    std::string post_type_;
    std::string excerpt_;
    std::string slug_;
    std::string featured_image_;
    std::vector<int> categories_;
    std::vector<int> tags_;
    int author_ = 0;
    int timeout_ = 20;
    int max_retries_ = 0;
    int retry_delay_ms_ = 1000;
    LogFn log_;
};

// Список доступных типов записей (включая пользовательские) для подсказки
// пользователю — возвращает «Название (slug)» через запятую.
std::string wordpress_list_types(HttpClient& client, const std::string& site_url) {
    const std::string ep = site_url + "/wp-json/wp/v2/types";
    NetworkConfig nc;
    nc.timeout_seconds = 10;
    HttpResponse r = client.get(ep, nc);
    if (!r.ok || r.status != 200) return "(не удалось получить список типов)";
    bool ok = false;
    Json j = Json::parse(r.body, &ok);
    if (!ok || !j.is_object()) return "(нет данных о типах)";
    std::string out;
    for (const std::string& key : j.keys()) {
        const Json& v = j.get(key);
        const std::string rb = v.get("rest_base").as_string();
        const std::string nm = v.get("name").as_string();
        if (!out.empty()) out += ", ";
        if (nm.empty()) out += key;
        else out += nm + " (" + (rb.empty() ? key : rb) + ")";
    }
    return out.empty() ? "(типов нет)" : out;
}

} // namespace

// Регистрируется в ll_plugin_init под "wordpress" (см. plugin_main.cpp).
std::unique_ptr<Sink> make_wordpress_sink(const SinkConfig& cfg, Storage& storage,
                                          const LogFn& log) {
    return std::make_unique<WordPressSink>(cfg, storage, log);
}

std::string wordpress_check_connection(const std::string& site_url,
                                       const std::string& user,
                                       const std::string& app_password) {
    const std::string su = rtrim(site_url, '/');
    std::string pass = app_password;
    {
        std::string norm;
        for (char c : pass) if (c != ' ') norm += c;
        pass = norm;
    }
    if (su.empty() || user.empty() || pass.empty())
        return "не заданы site_url / логин / пароль";

    HttpClient client;
    if (!client.init()) return "libcurl недоступен";

    const std::string endpoint = su + "/wp-json/wp/v2/users/me";
    const std::string auth = "Authorization: Basic " + base64_encode(user + ":" + pass);
    NetworkConfig nc;
    nc.timeout_seconds = 15;
    HttpResponse resp = client.get(endpoint, nc,
                                   std::vector<std::string>{auth});
    if (!resp.ok)
        return "сайт недоступен: " + (resp.error.empty() ? "нет ответа" : resp.error);
    if (resp.status == 200) {
        bool ok = false;
        Json j = Json::parse(resp.body, &ok);
        std::string name = ok ? j.get("name").as_string() : std::string();
        std::string result = "OK: авторизован" +
                             (name.empty() ? std::string("") : " как «" + name + "»");
        result += "\nТипы записей WP:\n" + wordpress_list_types(client, su);
        return result;
    }
    if (resp.status == 401 || resp.status == 403) {
        std::string body = resp.body;
        if (body.size() > 300) body = body.substr(0, 300) + "…";
        return "ошибка авторизации (HTTP " + std::to_string(resp.status) +
               "): " + (body.empty() ? "нет тела ответа (возможно, сервер "
                                       "не передаёт заголовок Authorization)"
                                     : body);
    }
    return "HTTP " + std::to_string(resp.status) +
           (resp.body.empty() ? std::string("") : ": " + resp.body);
}

} // namespace news_rewriter
