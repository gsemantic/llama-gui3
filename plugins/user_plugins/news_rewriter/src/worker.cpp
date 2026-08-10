#include "worker.h"

#include <chrono>
#include <ctime>

#include "extractor.h"

namespace news_rewriter {

Worker::Worker() : fetcher_(std::make_unique<Fetcher>()) {
    // Ретраи рерайта по умолчанию как в чате: 1 первичная + 2 повторные (всего
    // 3 попытки), короткий backoff 1с → 2с. Тесты могут переопределить.
    llm_retry_policy_.max_retries = 2;
    llm_retry_policy_.backoff_seconds = {1, 2};
}

Worker::~Worker() {
    stop_and_join();
}

bool Worker::start() {
    if (thread_.joinable()) return true;
    stop_.store(false);
    thread_done_.store(false);
    thread_ = std::thread(&Worker::loop, this);
    return true;
}

void Worker::stop_and_join() {
    if (!thread_.joinable()) return;
    stop_.store(true);
    cv_.notify_all();
    // Ограниченное ожидание: в норме поток выходит сразу. Если идёт длинный
    // рерайт/загрузка (непрерываемые вызовы), ждём его завершения, но логируем.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!thread_done_.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!thread_done_.load()) {
        log("предупреждение: поток не завершился за 5 с "
            "(возможно, идёт рерайт/загрузка), ждём завершения");
    }
    thread_.join();
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

void Worker::set_data_dir(const std::string& data_dir) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    data_dir_ = data_dir;
    if (!data_dir.empty()) storage_.init(data_dir);
}

void Worker::set_retry_policy(const RetryPolicy& retry) {
    // Хранит только backoff-задержки. Количество попыток берётся из
    // Config::max_retries (см. Worker::loop), поэтому тесты задают его в конфиге.
    std::lock_guard<std::mutex> lock(data_mutex_);
    retry_policy_ = retry;
}

void Worker::set_llm_retry_policy(const RetryPolicy& retry) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    llm_retry_policy_ = retry;
}

void Worker::debug_force_schedule_due() {
    post(Command{CmdType::DebugForceDue});
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

// Прерываемый сон: даёт реагировать на Stop (cancel_) даже во время backoff.
bool Worker::sleep_interruptible(const std::chrono::seconds& delay) {
    const auto deadline = std::chrono::steady_clock::now() + delay;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancel_.load() || stop_.load()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
}

// Убирает устаревшую Error-заглушку источника (созданную при повторяемом сбое),
// если последующий ретрай источника завершился успешно.
void Worker::remove_stale_source_error(const std::string& url) {
    const std::string id = sha256_hex(url);
    std::lock_guard<std::mutex> lock(data_mutex_);
    for (auto it = pipeline_.begin(); it != pipeline_.end(); ++it) {
        if (it->id == id) {
            pipeline_.erase(it);
            break;
        }
    }
    for (auto it = state_.articles.begin(); it != state_.articles.end(); ++it) {
        if (it->url == url) {
            state_.articles.erase(it);
            break;
        }
    }
}

namespace {
std::chrono::seconds llm_backoff_delay(const RetryPolicy& rp, int attempt) {
    if (rp.backoff_seconds.empty()) return std::chrono::seconds(0);
    std::size_t idx = static_cast<std::size_t>(attempt);
    if (idx >= rp.backoff_seconds.size()) idx = rp.backoff_seconds.size() - 1;
    return std::chrono::seconds(rp.backoff_seconds[idx]);
}
} // namespace

// Рерайт статьи через LLM с ретраями (как в чате: 1 первичная + max_retries
// повторных, короткий backoff). Ошибки LLM транзиентны (модель могла не
// ответить), поэтому не отбрасываем статью с первой попытки. Возвращает false,
// когда попытки исчерпаны или обход прерван пользователем (a.error заполнена).
bool Worker::rewrite(Article& a, const Config& cfg) {
    if (!llm_) return true;   // рерайт не настроен — экспортируем оригинал

    a.status = TaskStatus::Rewriting;
    set_status(a, "рерайт: " + a.title_original);

    RetryPolicy rp;
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        rp = llm_retry_policy_;
    }
    if (rp.max_retries < 0) rp.max_retries = 0;
    const int max_attempts = 1 + rp.max_retries;

    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        const RewriteResult rr = rewrite_article(a, cfg.rewrite, llm_);
        a.retry_count = static_cast<uint32_t>(attempt);
        if (rr.ok) {
            a.title_rewritten = rr.title;
            a.body_rewritten = rr.body;
            return true;
        }
        a.error = rr.error;
        if (attempt == max_attempts - 1) break;

        const std::chrono::seconds delay = llm_backoff_delay(rp, attempt);
        log("рерайт: попытка " + std::to_string(attempt + 1) + "/" +
            std::to_string(max_attempts) + " не удалась («" + a.title_original +
            "» — " + rr.error + "), повтор через " + std::to_string(delay.count()) + " с");
        if (!sleep_interruptible(delay)) {
            a.error = "обход прерван пользователем";
            return false;
        }
    }
    return false;
}

// Полный экспорт статьи: рерайт (если LLM настроен) + запись через активный
// Sink + дедупликация. Возвращает true, если статья успешно завершена.
bool Worker::export_article(const Config& cfg, Article& a) {
    // Рерайт (с ретраями, как в чате)
    if (!rewrite(a, cfg)) {
        a.status = TaskStatus::Error;
        return false;
    }

    // Экспорт через активный Sink
    a.status = TaskStatus::Exporting;
    set_status(a, "экспорт: " + a.title_original);

    if (!storage_.ready()) {
        a.status = TaskStatus::Error;
        a.error = "хранилище не инициализировано (не задан каталог данных)";
        return false;
    }

    if (storage_.is_duplicate(a)) {
        a.status = TaskStatus::Done;
        log("дубликат пропущен: " + a.title_original);
        return true;
    }

    std::unique_ptr<Sink> sink = SinkRegistry::instance().create(
        cfg.sink, storage_, log_callback_);
    if (!sink) {
        a.status = TaskStatus::Error;
        a.error = "неизвестный тип sink: " + cfg.sink.type;
        return false;
    }

    if (!sink->write(a)) {
        a.status = TaskStatus::Error;
        if (a.error.empty()) a.error = "ошибка записи sink";
        return false;
    }

    storage_.mark_written(a);
    a.status = TaskStatus::Done;
    return true;
}

void Worker::loop() {
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        state_.worker_active = true;
    }

    for (;;) {
        // Применяем текущий конфиг/политику ретраев к планировщику и узнаём,
        // сколько ждать до следующего авто-запуска. Количество повторных
        // попыток берётся из Config::max_retries (пользовательская настройка);
        // backoff-задержки — из retry_policy_ (тесты задают 0 для скорости).
        Config cfg_now;
        RetryPolicy rp;
        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            cfg_now = config_;
            rp = retry_policy_;
            rp.max_retries = config_.max_retries;
        }
        scheduler_.configure(cfg_now, rp);
        const std::chrono::seconds delay = scheduler_.next_delay();
        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            state_.scheduled = scheduler_.schedule_active();
            state_.next_run_in_seconds =
                scheduler_.schedule_active() ? static_cast<int>(delay.count()) : -1;
        }

        Command cmd;
        bool have_cmd = false;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait_for(lock, delay,
                         [this] { return stop_.load() || !queue_.empty(); });
            if (stop_.load() && queue_.empty()) break;
            if (!queue_.empty()) {
                cmd = std::move(queue_.front());
                queue_.pop();
                have_cmd = true;
            }
        }

        if (have_cmd) {
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
                case CmdType::DebugForceDue:
                    scheduler_.force_due();   // тесты: следующий авто-запуск сразу
                    break;
            }
        } else if (!stop_.load() && scheduler_.due()) {
            // Авто-запуск по расписанию (таймер истёк, команд нет).
            log("автоматический запуск по расписанию");
            process_run(get_config());
        }
    }

    thread_done_.store(true);
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        state_.worker_active = false;
        state_.scheduled = false;
        state_.next_run_in_seconds = -1;
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
    scheduler_.note_run_started();   // любой запуск сдвигает авто-расписание

    // Выходная папка: пользовательская (cfg.sink.output_dir) или дефолтная.
    // Инициализируем Storage перед обходом — каталог данных мог измениться
    // после ReloadConfig.
    std::string storage_warn;
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        if (!cfg.sink.output_dir.empty()) {
            if (!storage_.init_root(cfg.sink.output_dir)) {
                storage_warn = "не удалось создать выходной каталог: " + cfg.sink.output_dir;
            }
        } else if (!data_dir_.empty()) {
            storage_.init(data_dir_);
        }
    }
    if (!storage_warn.empty()) log("предупреждение: " + storage_warn);

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

        // Источник: первичная попытка + ретраи с backoff при сетевых сбоях.
        uint32_t retries = 0;
        for (;;) {
            if (cancel_.load()) {
                log("обход прерван пользователем");
                break;
            }
            if (process_source(cfg, src, retries)) {
                processed++;
                break;
            }
            if (!scheduler_.can_retry(retries)) {
                log("источник " + src.url + ": попытки исчерпаны");
                break;
            }
            const auto delay = scheduler_.retry_delay(retries);
            log("источник " + src.url + ": повторная попытка " +
                std::to_string(retries + 1) + " из " +
                std::to_string(scheduler_.retry_policy().max_retries) +
                " через " + std::to_string(delay.count()) + " с");
            if (!sleep_interruptible(delay)) {
                log("обход прерван пользователем");
                break;
            }
            retries++;
        }
        if (cancel_.load()) break;
    }

    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        state_.running = false;
        state_.pending_tasks = 0;

        // Итог по статьям: успешные и с ошибкой — для честного отчёта в UI.
        int done = 0;
        int errors = 0;
        for (const auto& v : state_.articles) {
            if (v.status == TaskStatus::Done) done++;
            else if (v.status == TaskStatus::Error) errors++;
        }
        state_.done_count = done;
        state_.error_count = errors;

        const std::string summary =
            "источников: " + std::to_string(processed) +
            ", статей: " + std::to_string(done) + ", ошибок: " + std::to_string(errors);
        if (cancel_.load()) {
            state_.last_message = "обход прерван пользователем (" + summary + ")";
        } else if (errors > 0) {
            state_.last_message = "обход завершён с ошибками (" + summary + ")";
        } else {
            state_.last_message = "обход завершён (" + summary + ")";
        }
    }
}

bool Worker::process_source(const Config& cfg, const SourceConfig& src, uint32_t retries) {
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
        a.retry_count = retries;
        a.error = "загрузчик недоступен (libcurl не найден)";
        set_status(a, "ошибка загрузки: " + src.url + " — " + a.error);
        return true;   // постоянный сбой: ретраи бессмысленны
    }

    const FetchResult res = fetcher_->fetch(src.url, src.type, cfg.network);
    if (!res.ok) {
        Article a = make_source_article();
        a.status = TaskStatus::Error;
        a.retry_count = retries;
        a.error = res.error;
        set_status(a, "ошибка загрузки: " + src.url + " — " + res.error);
        return false;  // повторяемый сбой (сеть/таймаут) → caller ретраит
    }

    // Источник, который раньше падал с Error-заглушкой, теперь успешен:
    // убираем устаревшую заглушку из снапшота.
    remove_stale_source_error(src.url);

    if (src.type == "page") {
        Article a = make_source_article();
        a.status = TaskStatus::Extracting;
        set_status(a, "извлечение текста: " + src.url);
        const ExtractedArticle ex = extract_page(res.html, src.extract);
        a.title_original = ex.title;
        a.body_original = ex.body;
        a.content_hash = sha256_hex(a.title_original + "\n" + a.body_original);
        if (!export_article(cfg, a)) {
            set_status(a, "ошибка обработки: " + src.url + " — " + a.error);
            return true;
        }
        set_status(a, "страница: " + (a.title_original.empty()
                                          ? src.url : a.title_original));
        return true;
    }

    // rss/atom: каждая новость — отдельная статья. Применяем пользовательские
    // ограничения: свежесть (max_age_hours) и лимит статей с источника.
    int accepted = 0;
    for (const auto& item : res.items) {
        if (cancel_.load()) {
            log("обход прерван пользователем");
            break;
        }

        // Свежесть: статьи старше max_age_hours пропускаем. Если дату не
        // удалось распознать (pub_date пуст/невалиден), статью не фильтруем.
        if (cfg.max_age_hours > 0) {
            const std::int64_t pub = parse_feed_time(item.pub_date);
            if (pub > 0) {
                const std::int64_t now_sec = static_cast<std::int64_t>(std::time(nullptr));
                if (now_sec - pub > static_cast<std::int64_t>(cfg.max_age_hours) * 3600) {
                    continue;   // слишком старая новость
                }
            }
        }

        if (cfg.max_items_per_source > 0 && accepted >= cfg.max_items_per_source) {
            break;   // взяли достаточно статей с этого источника
        }
        accepted++;

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
        if (!export_article(cfg, a)) {
            set_status(a, "ошибка обработки: " + a.title_original + " — " + a.error);
            continue;
        }
        set_status(a, "новость: " + a.title_original);
    }

    log("источник: " + src.url + " — новостей: " + std::to_string(res.items.size()) +
        (cfg.max_items_per_source > 0 ? " (взято: " + std::to_string(accepted) + ")" : ""));
    return true;
}

} // namespace news_rewriter
