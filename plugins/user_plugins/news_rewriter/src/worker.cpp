#include "worker.h"

#include <chrono>

#include "extractor.h"

namespace news_rewriter {

Worker::Worker() : fetcher_(std::make_unique<Fetcher>()) {}

Worker::~Worker() {
    stop_and_join();
}

bool Worker::start() {
    if (thread_.joinable()) return true;
    stop_.store(false);
    thread_ = std::thread(&Worker::loop, this);
    return true;
}

void Worker::stop_and_join() {
    if (!thread_.joinable()) return;
    stop_.store(true);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Worker::post(Command cmd) {
    if (cmd.type == CmdType::Stop) {
        cancel_.store(true);
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push(std::move(cmd));
    }
    cv_.notify_all();
}

void Worker::set_config(const Config& cfg) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    config_ = cfg;
}

Config Worker::get_config() const {
    std::lock_guard<std::mutex> lock(data_mutex_);
    return config_;
}

WorkerState Worker::snapshot() const {
    std::lock_guard<std::mutex> lock(data_mutex_);
    return state_;
}

void Worker::set_log_callback(LogFn cb) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    log_callback_ = std::move(cb);
}

void Worker::set_fetcher(std::unique_ptr<IFetch> fetcher) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    fetcher_ = std::move(fetcher);
}

void Worker::set_llm(LlmFn llm) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    llm_ = std::move(llm);
}

void Worker::log(const std::string& msg) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    state_.last_message = msg;
    if (log_callback_) log_callback_(msg);
}

namespace {

ArticleStatusView view_of(const Article& a) {
    return ArticleStatusView{a.url, a.source, a.title_original, a.status, a.error, a.retry_count};
}

} // namespace

void Worker::set_status(const Article& a, const std::string& msg) {
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        bool found = false;
        for (auto& p : pipeline_) {
            if (p.id == a.id) {
                p = a;
                found = true;
                break;
            }
        }
        if (!found) pipeline_.push_back(a);

        bool view_found = false;
        for (auto& v : state_.articles) {
            if (v.url == a.url) {
                v = view_of(a);
                view_found = true;
                break;
            }
        }
        if (!view_found) state_.articles.push_back(view_of(a));
    }
    log(msg);
}

// Рерайт статьи через LLM (если настроен). Возвращает true, если рерайт
// успешен или LLM не нужен; false — при ошибке (статья помечена Error).
bool Worker::rewrite(Article& a, const Config& cfg) {
    if (!llm_) {
        a.status = TaskStatus::Done;
        return true;
    }
    a.status = TaskStatus::Rewriting;
    set_status(a, "рерайт: " + a.title_original);
    const RewriteResult rr = rewrite_article(a, cfg.rewrite, llm_);
    if (!rr.ok) {
        a.status = TaskStatus::Error;
        a.error = rr.error;
        return false;
    }
    a.title_rewritten = rr.title;
    a.body_rewritten = rr.body;
    a.status = TaskStatus::Done;
    return true;
}

void Worker::loop() {
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        state_.worker_active = true;
    }

    for (;;) {
        Command cmd;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait(lock, [this] { return stop_.load() || !queue_.empty(); });
            if (stop_.load() && queue_.empty()) break;
            if (queue_.empty()) continue;
            cmd = std::move(queue_.front());
            queue_.pop();
        }

        switch (cmd.type) {
            case CmdType::RunNow:
                process_run(get_config());
                break;
            case CmdType::ReloadConfig: {
                Config cfg = config_from_json(Json::parse(cmd.arg));
                set_config(cfg);
                log("конфигурация обновлена");
                break;
            }
            case CmdType::Stop:
                log("обход прерван пользователем");
                break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        state_.worker_active = false;
    }
}

void Worker::process_run(const Config& cfg) {
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        if (state_.running) {
            state_.last_message = "обход уже выполняется";
            return;
        }
        state_.running = true;
        pipeline_.clear();
        state_.articles.clear();
        state_.pending_tasks = 0;
        state_.last_run_unix = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }
    cancel_.store(false);

    if (fetcher_ && !fetcher_->is_available()) {
        fetcher_->init();
    }

    int processed = 0;
    for (const auto& src : cfg.sources) {
        if (!src.enabled) continue;
        if (cancel_.load()) {
            log("обход прерван пользователем");
            break;
        }
        process_source(cfg, src);
        processed++;
    }

    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        state_.running = false;
        state_.pending_tasks = 0;
        state_.last_message = "обход завершён (источников: " + std::to_string(processed) + ")";
    }
}

void Worker::process_source(const Config& cfg, const SourceConfig& src) {
    auto make_source_article = [&]() {
        Article a;
        a.url = src.url;
        a.id = sha256_hex(src.url);
        a.source = host_of(src.url);
        a.fetched_at = iso8601_now();
        a.language = cfg.rewrite.language;
        return a;
    };

    log("загрузка: " + src.url);

    if (!fetcher_ || !fetcher_->is_available()) {
        Article a = make_source_article();
        a.status = TaskStatus::Error;
        a.error = "загрузчик недоступен (libcurl не найден)";
        set_status(a, "ошибка загрузки: " + src.url + " — " + a.error);
        return;
    }

    const FetchResult res = fetcher_->fetch(src.url, src.type, cfg.network);
    if (!res.ok) {
        Article a = make_source_article();
        a.status = TaskStatus::Error;
        a.error = res.error;
        set_status(a, "ошибка загрузки: " + src.url + " — " + res.error);
        return;
    }

    if (src.type == "page") {
        Article a = make_source_article();
        a.status = TaskStatus::Extracting;
        set_status(a, "извлечение текста: " + src.url);
        const ExtractedArticle ex = extract_page(res.html, src.extract);
        a.title_original = ex.title;
        a.body_original = ex.body;
        a.content_hash = sha256_hex(a.title_original + "\n" + a.body_original);
        if (!rewrite(a, cfg)) {
            set_status(a, "ошибка рерайта: " + src.url + " — " + a.error);
            return;
        }
        set_status(a, "страница: " + (a.title_original.empty()
                                          ? src.url : a.title_original));
        return;
    }

    // rss/atom: каждая новость — отдельная статья.
    for (const auto& item : res.items) {
        Article a;
        a.url = item.link.empty() ? src.url : item.link;
        a.id = sha256_hex(a.url);
        a.source = host_of(src.url);
        a.fetched_at = iso8601_now();
        a.language = cfg.rewrite.language;
        a.status = TaskStatus::Extracting;
        const ExtractedArticle ex = extract_from_description(item.description);
        a.title_original = html_to_text(item.title);
        a.body_original = ex.body;
        a.content_hash = sha256_hex(a.title_original + "\n" + a.body_original);
        if (!rewrite(a, cfg)) {
            set_status(a, "ошибка рерайта: " + a.title_original + " — " + a.error);
            continue;
        }
        set_status(a, "новость: " + a.title_original);
    }

    log("источник: " + src.url + " — новостей: " + std::to_string(res.items.size()));
}

} // namespace news_rewriter
