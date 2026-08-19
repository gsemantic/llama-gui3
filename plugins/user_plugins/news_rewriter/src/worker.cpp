#include "worker.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <set>
#include <vector>

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
    // Будим ожидание предпросмотра (на случай Stop/отмены во время разведки).
    proposal_cv_.notify_all();
}

void Worker::proposal_reply(ProposalResp r) {
    {
        std::lock_guard<std::mutex> lock(proposal_mutex_);
        proposal_response_ = r;
    }
    proposal_cv_.notify_all();
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
    // Оборачиваем вызов LLM паузой, чтобы не «долбить» облако подряд и не
    // упираться в лимит частоты провайдера (429) при обходе списка новостей.
    llm_ = [this, base = std::move(llm)](const std::string& p, std::string& out,
                                         std::string& err) -> bool {
        if (llm_call_interval_ > std::chrono::milliseconds(0))
            std::this_thread::sleep_for(llm_call_interval_);
        return base(p, out, err);
    };
}

void Worker::set_data_dir(const std::string& data_dir) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    data_dir_ = data_dir;
    if (!data_dir.empty()) storage_.init(data_dir);
    load_learned();
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
    state_.last_message = ensure_utf8(msg);
    if (log_callback_) log_callback_(msg);
}

namespace {

ArticleStatusView view_of(const Article& a) {
    ArticleStatusView v;
    v.url = a.url;
    v.source = a.source;
    v.title = a.title_original;
    v.source_image = a.source_image;
    v.published_at = a.published_at;
    v.status = a.status;
    v.error = a.error;
    v.retry_count = a.retry_count;
    return v;
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

// Убирает из URL трекинг-параметры (erid, utm_*, fbclid, yclid, _ga, ref и
// т.п.), чтобы ссылка статьи была стабильной и корректно дедуплицировалась
// (иначе одна и та же статья с разными метками считается разными, а WordPress
// часто не принимает «?erid=...» в slug).
std::string strip_tracking_params(const std::string& url) {
    const std::size_t q = url.find('?');
    if (q == std::string::npos) return url;
    const std::string base = url.substr(0, q);
    const std::string query = url.substr(q + 1);
    static const char* kDrop[] = {
        "erid", "utm_source", "utm_medium", "utm_campaign", "utm_term",
        "utm_content", "fbclid", "gclid", "yclid", "ym_", "_ga", "ref",
        "roistat", "sub", "yclid"
    };
    std::vector<std::string> keep;
    std::size_t pos = 0;
    while (pos < query.size()) {
        std::size_t amp = query.find('&', pos);
        const std::string pair = query.substr(pos, amp == std::string::npos
                                                      ? std::string::npos : amp - pos);
        std::size_t eq = pair.find('=');
        const std::string key = eq == std::string::npos
                                    ? pair : pair.substr(0, eq);
        bool drop = false;
        for (const char* k : kDrop) {
            if (key == k || key.rfind(k, 0) == 0) { drop = true; break; }
        }
        if (!drop && !pair.empty()) keep.push_back(pair);
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    if (keep.empty()) return base;
    std::string out = base + "?";
    for (std::size_t i = 0; i < keep.size(); ++i) {
        if (i) out += '&';
        out += keep[i];
    }
    return out;
}

// Распознавание rate-limit по тексту ошибки облака (код 1305/1302, "Rate
// limit", китайские сообщения Zhipu и т.п.). Используется, чтобы делать
// длинную паузу вместо агрессивного ретрая и не исчерпывать квоту ещё сильнее.
bool is_rate_limit_error(const std::string& err) {
    const std::string e = err;
    auto has = [&e](const char* sub) {
        return e.find(sub) != std::string::npos;
    };
    return has("1305") || has("1302") || has("429") ||
           has("Rate limit") || has("rate limit") ||
           has("лимит запрос") || has("Превышен лимит") ||
           has("访问量过大") || has("速率限制");
}

// Длинная пауза при rate-limit: аккаунт/модель перегружены, короткий backoff
// (1-2 с) только ухудшает — квота исчерпывается (код 1302). 30с → 60с → 120с.
std::chrono::seconds rate_limit_backoff(int attempt) {
    static const std::vector<int> b{30, 60, 120};
    std::size_t idx = static_cast<std::size_t>(attempt);
    if (idx >= b.size()) idx = b.size() - 1;
    return std::chrono::seconds(b[idx]);
}
} // namespace

// Рерайт статьи через LLM с ретраями (как в чате: 1 первичная + max_retries
// повторных, короткий backoff). Ошибки LLM транзиентны (модель могла не
// ответить), поэтому не отбрасываем статью с первой попытки. Возвращает false,
// когда попытки исчерпаны или обход прерван пользователем (a.error заполнена).
bool Worker::rewrite(Article& a, const Config& cfg) {
    if (!llm_) return true;   // рерайт не настроен — экспортируем оригинал

    // Не отправляем пустой/нераспознанный материал в LLM: перегруженная модель
    // на пустом входе «сочиняет» канонический текст, который иначе ушёл бы в
    // публикацию. Лучше пропустить статью, чем выложить сгенерированный мусор.
    if (a.body_original.empty()) {
        a.error = "пустое тело статьи — рерайт невозможен, пропускаем";
        return false;
    }

    a.status = TaskStatus::Rewriting;
    set_status(a, "рерайт: " + a.title_original);

    RetryPolicy rp;
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        rp = llm_retry_policy_;
    }
    if (rp.max_retries < 0) rp.max_retries = 0;
    const int max_attempts = 1 + rp.max_retries;

    const bool seo_enabled = cfg.rewrite.seo.enabled && llm_;
    // Комбинированный путь: рерайт + SEO в ОДНОМ облачном вызове (экономит
    // квоту/лимиты). Отключается, если SEO выключен, запрещён combine или уже
    // пропущен из-за rate-limit в этом обходе.
    const bool combine = seo_enabled && cfg.rewrite.seo.combine_with_rewrite &&
                         !seo_skipped_.load();

    auto apply_seo = [&](const SeoResult& sr) {
        if (sr.ok) {
            a.seo_focus_keyword = sr.focus_keyword;
            a.seo_meta_description = sr.meta_description;
            a.seo_title = sr.seo_title;
        } else {
            // SEO — best-effort: не роняем статью. При rate-limit отключаем SEO
            // на остаток обхода, чтобы не долбить облако и не тратить квоту.
            log("ВНИМАНИЕ: SEO не сгенерирован («" + a.title_original +
                "» — " + sr.error +
                "); статья будет опубликована БЕЗ SEO-мета");
            run_seo_missing_.fetch_add(1);
            if (is_rate_limit_error(sr.error)) seo_skipped_.store(true);
        }
    };

    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        a.retry_count = static_cast<uint32_t>(attempt);
        RewriteResult rr;

        if (combine) {
            // ОДИН вызов: рерайт + SEO сразу.
            const RewriteSeoResult cr = rewrite_and_seo(a, cfg.rewrite, llm_);
            if (cr.ok && !cr.title.empty() && !cr.body.empty()) {
                a.title_rewritten = cr.title;
                a.body_rewritten = cr.body;
                if (a.title_rewritten.empty()) a.title_rewritten = a.title_original;
                apply_seo(cr.seo);
                return true;
            }
            // Комбинированный вызов не дал валидного рерайта.
            if (is_rate_limit_error(cr.error)) {
                // Rate-limit: НЕ раздуваем в 2 отдельных вызова — это лишь
                // усугубляет перегруз модели (код 1305 = «модель перегружена»).
                // Отключаем SEO на остаток обхода и выходим из ветки; внешний
                // цикл сделает длинную паузу (30/60/120с) и повторит ТОТ ЖЕ
                // комбинированный запрос, который дешевле двух отдельных.
                seo_skipped_.store(true);
                a.error = cr.error;
            } else {
                // Не rate-limit (напр. модель не выдала корректный JSON): пробуем
                // два отдельных, более простых вызова (рерайт, затем SEO).
                rr = rewrite_article(a, cfg.rewrite, llm_);
                if (rr.ok) {
                    a.title_rewritten = rr.title;
                    a.body_rewritten = rr.body;
                    if (a.title_rewritten.empty()) a.title_rewritten = a.title_original;
                    if (seo_enabled && !seo_skipped_.load()) {
                        apply_seo(generate_seo(a, cfg.rewrite.seo, llm_));
                    }
                    return true;
                }
                a.error = cr.error.empty() ? rr.error : cr.error;
            }
        } else {
            // Обычный путь: рерайт (1 вызов) + при необходимости SEO (2-й вызов).
            rr = rewrite_article(a, cfg.rewrite, llm_);
            if (rr.ok) {
                a.title_rewritten = rr.title;
                a.body_rewritten = rr.body;
                if (a.title_rewritten.empty()) a.title_rewritten = a.title_original;
                if (seo_enabled && !seo_skipped_.load()) {
                    apply_seo(generate_seo(a, cfg.rewrite.seo, llm_));
                }
                return true;
            }
            a.error = rr.error;
        }

        if (attempt == max_attempts - 1) break;

        // Rate-limit требует длинной паузы; иначе — обычный backoff рерайта.
        const bool rate = is_rate_limit_error(a.error);
        const std::chrono::seconds delay =
            rate ? rate_limit_backoff(attempt) : llm_backoff_delay(rp, attempt);
        log("рерайт: попытка " + std::to_string(attempt + 1) + "/" +
            std::to_string(max_attempts) + " не удалась («" + a.title_original +
            "» — " + a.error + ")" +
            (rate ? ", rate-limit — длинная пауза" : "") +
            ", повтор через " + std::to_string(delay.count()) + " с");
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

    // Sink создаём ДО проверки дедупа, чтобы он мог свериться с реальным
    // состоянием приёмника (сайт/хранилище), а не только с локальным индексом.
    // Передаём каталог данных плагина в sink (для пути к .env с секретами),
    // чтобы он не зависел от output_dir.
    SinkConfig sink_cfg = cfg.sink;
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        sink_cfg.data_dir = data_dir_;
    }
    std::unique_ptr<Sink> sink = SinkRegistry::instance().create(
        sink_cfg, storage_, log_callback_);
    if (!sink) {
        a.status = TaskStatus::Error;
        a.error = "неизвестный тип sink: " + cfg.sink.type;
        return false;
    }

    // Дедуп с учётом реального состояния приёмника.
    const Presence p = sink->presence(a);
    if (p == Presence::Present) {
        // Уже опубликовано в приёмнике — не дублируем, держим индекс в актуале.
        a.status = TaskStatus::Done;
        storage_.mark_written(a);
        log("пропущено: уже есть в приёмнике («" + a.title_original + "»)");
        return true;
    }
    if (storage_.is_duplicate(a)) {
        if (p == Presence::Absent) {
            // Локально помечено, но в приёмнике НЕТ (удалили вручную) —
            // сбрасываем устаревшую метку и переиздаём.
            storage_.forget(a);
            log("локальная метка дедупа устарела (нет в приёмнике), "
                "переиздаём: " + a.title_original);
        } else {
            // Unknown: состояние приёмника недоступно (нет связи/прав/опция
            // выключена) — консервативно пропускаем, чтобы не создать дубликат.
            a.status = TaskStatus::Done;
            log("дубликат пропущен (состояние приёмника недоступно): " +
                a.title_original);
            return true;
        }
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
        state_.seo_missing = 0;
        run_seo_missing_.store(0);
        seo_skipped_.store(false);
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

        const int seo_missing = run_seo_missing_.load();
        state_.seo_missing = seo_missing;
        const std::string summary =
            "источников: " + std::to_string(processed) +
            ", статей: " + std::to_string(done) + ", ошибок: " + std::to_string(errors) +
            (seo_missing > 0 ? ", без SEO: " + std::to_string(seo_missing) : "");
        if (cancel_.load()) {
            state_.last_message = "обход прерван пользователем (" + summary + ")";
        } else if (errors > 0) {
            state_.last_message = "обход завершён с ошибками (" + summary + ")";
        } else if (seo_missing > 0) {
            state_.last_message =
                "обход завершён: статьи опубликованы БЕЗ SEO-мета (" + summary + ")";
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
        return res.permanent;  // false (повторяемый) → caller ретраит; true (постоянный) → стоп
    }

    // Источник, который раньше падал с Error-заглушкой, теперь успешен:
    // убираем устаревшую заглушку из снапшота.
    remove_stale_source_error(src.url);

    if (src.type == "page") {
        // Режим предпросмотра (разведки): вместо авто-извлечения и сразу рерайта
        // сначала предлагаем пользователю варианты текста/фото и ждём явного
        // одобрения. Старый (без подтверждения) поток этим не затрагивается.
        if (src.preview) {
            const bool ok = recon_and_confirm(cfg, src, res.html);
            if (!ok && !cancel_.load()) {
                Article a = make_source_article();
                a.status = TaskStatus::Error;
                a.retry_count = retries;
                a.error = "предпросмотр: варианты не одобрены";
                set_status(a, "ошибка обработки: " + src.url + " — " + a.error);
            }
            return true;  // пользовательское решение — повторять бессмысленно
        }

        // Страница может быть либо одной статьёй, либо списком/категорией.
        // extract_page_items возвращает 1 элемент для одиночной статьи и N —
        // для списка (каждый со своим url/заголовком/картинкой).
        std::vector<ExtractedArticle> items =
            extract_page_items(res.html, src.url, src.extract);
        if (items.empty()) {
            Article a = make_source_article();
            a.status = TaskStatus::Error;
            a.error = "не удалось извлечь текст страницы";
            set_status(a, "ошибка обработки: " + src.url + " — " + a.error);
            return true;
        }

        // Список/категория, если среди извлечённых элементов есть реальные
        // ссылки на статьи: обходим каждую (подгружаем её страницу и берём
        // картинку/ссылку именно статьи, а не листинга). Иначе — это
        // действительно одиночная статья: используем страницу как есть.
        bool list_has_link = false;
        for (const auto& it : items) {
            if (!it.url.empty()) { list_has_link = true; break; }
        }
        if (items.size() == 1 && !list_has_link) {
            // Одиночная статья — без доп. загрузок.
            Article a = make_source_article();
            a.status = TaskStatus::Extracting;
            set_status(a, "извлечение текста: " + src.url);
            const ExtractedArticle& ex = items.front();
            a.title_original = ex.title;
            a.body_original = ex.body;
            // Относительную ссылку на фото резолвим в абсолютную по URL источника.
            a.source_image = resolve_url(ex.image, src.url);
            a.content_hash =
                sha256_hex(a.title_original + "\n" + a.body_original);
            if (!export_article(cfg, a)) {
                set_status(a, "ошибка обработки: " + src.url + " — " + a.error);
                return true;
            }
            set_status(a, "страница: " + (a.title_original.empty()
                                              ? src.url : a.title_original));
            return true;
        }

        // Список: краулим каждую статью — подгружаем её реальную страницу и
        // извлекаем полный текст + свою hero-картинку. При сбое загрузки
        // откатываемся на сниппет/картинку с листинга.
        // Свежие сверху: сначала те, у кого удалось распознать дату публикации.
        std::stable_sort(items.begin(), items.end(),
                         [](const ExtractedArticle& x, const ExtractedArticle& y) {
                             return x.published_at > y.published_at;
                         });
        // Один и тот же материал часто встречается в листинге несколько раз
        // (ссылка на картинку + на заголовок, либо desktop/mobile варианты) с
        // одинаковым URL — убираем дубликаты, иначе они впустую тратят квоту
        // max_items_per_source и один из них отбрасывается дедупом приёмника.
        {
            std::vector<ExtractedArticle> uniq;
            std::set<std::string> seen;
            for (auto& it : items) {
                const std::string u = strip_tracking_params(it.url);
                if (u.empty()) {
                    uniq.push_back(std::move(it));
                    continue;
                }
                if (seen.insert(u).second) uniq.push_back(std::move(it));
            }
            items = std::move(uniq);
        }
        int accepted = 0;
        int no_url_idx = 0;
        for (const auto& item : items) {
            if (cancel_.load()) {
                log("обход прерван пользователем");
                break;
            }
            if (cfg.max_items_per_source > 0 && accepted >= cfg.max_items_per_source) {
                break;
            }

            // Свежесть (page-режим): статьи старше max_age_hours пропускаем,
            // если удалось распознать дату публикации на листинге.
            if (cfg.max_age_hours > 0 && item.published_at > 0) {
                const std::int64_t now_sec =
                    static_cast<std::int64_t>(std::time(nullptr));
                if (now_sec - item.published_at >
                    static_cast<std::int64_t>(cfg.max_age_hours) * 3600) {
                    log("пропущена старая новость: " +
                        (item.title.empty() ? item.url : item.title));
                    continue;
                }
            }

            ++accepted;

            Article a;
            if (item.url.empty()) {
                // Нет ссылки — берём сниппет с листинга, url делаем уникальным.
                a.url = src.url + "#item" + std::to_string(++no_url_idx);
            } else {
                a.url = strip_tracking_params(item.url);
            }
            a.id = sha256_hex(a.url);
            a.source = host_of(src.url);
            a.fetched_at = iso8601_now();
            a.published_at = item.published_at;
            a.language = cfg.rewrite.language;
            a.status = TaskStatus::Extracting;
            set_status(a, "извлечение: " + (item.title.empty() ? a.url : item.title));

            ExtractedArticle ex = item;
            if (!item.url.empty()) {
                const FetchResult ar = fetcher_->fetch(item.url, "page", cfg.network);
                if (ar.ok) ex = extract_page(ar.html, item.url, src.extract);
                // иначе оставляем сниппет с листинга
            }
            // Относительную ссылку на фото резолвим в абсолютную по URL статьи.
            if (!ex.image.empty()) ex.image = resolve_url(ex.image, item.url);
            a.title_original = ex.title;
            a.body_original = ex.body;
            // Картинка: предпочитаем hero со страницы статьи, иначе с листинга.
            a.source_image = ex.image.empty() ? item.image : ex.image;
            a.content_hash =
                sha256_hex(a.title_original + "\n" + a.body_original);

            if (!export_article(cfg, a)) {
                set_status(a, "ошибка обработки: " +
                                  (item.title.empty() ? a.url : item.title) +
                                  " — " + a.error);
                continue;
            }
            set_status(a, "статья: " + (a.title_original.empty()
                                            ? a.url : a.title_original));
        }
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
        a.published_at = parse_feed_time(item.pub_date);
        a.language = cfg.rewrite.language;
        a.status = TaskStatus::Extracting;
        const ExtractedArticle ex = extract_from_description(item.description);
        a.title_original = html_to_text(item.title);
        a.body_original = ex.body;
        a.source_image = item.image;   // заглавное изображение из ленты (media:content/enclosure/itunes)
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

std::string Worker::truncate(const std::string& s, std::size_t n) {
    if (s.size() <= n) return s;
    return s.substr(0, n) + "…";
}

void Worker::load_learned() {
    learned_strategy_.clear();
    if (data_dir_.empty()) return;
    const std::string path = data_dir_ + "/news_rewriter/preview_schemas.json";
    std::ifstream f(path);
    if (!f) return;
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    bool ok = false;
    std::string err;
    const Json j = Json::parse(content, &ok, &err);
    if (ok && j.is_object()) {
        for (const std::string& k : j.keys()) {
            learned_strategy_[k] = static_cast<int>(j.get(k).as_int(0));
        }
    }
}

void Worker::save_learned() {
    if (data_dir_.empty()) return;
    const std::string dir = data_dir_ + "/news_rewriter";
    Json j = Json::object();
    for (const auto& kv : learned_strategy_) j[kv.first] = kv.second;
    std::ofstream f(dir + "/preview_schemas.json");
    if (f) f << j.dump();
}

bool Worker::recon_and_confirm(const Config& cfg, const SourceConfig& src,
                               const std::string& html) {
    // Вспомогательная лямбда: показывает пользователю один вариант
    // (заголовок/фото/текст) и ждёт решения. Возвращает ответ или None, если
    // обход был прерван (тогда вызывающий видит cancel_).
    auto present = [&](const ExtractedArticle& art, int index, int total,
                       const std::string& strat) -> ProposalResp {
        {
            std::lock_guard<std::mutex> lk(data_mutex_);
            state_.proposal_active = true;
            state_.proposal.source_url = src.url;
            state_.proposal.title = ensure_utf8(art.title);
            state_.proposal.image = ensure_utf8(art.image);
            state_.proposal.body_preview = ensure_utf8(truncate(art.body, 600));
            state_.proposal.candidate_index = index;
            state_.proposal.candidate_total = total;
        }
        log("предпросмотр: вариант " + std::to_string(index) + "/" +
            std::to_string(total) + " (" + strat + ") — ожидание решения");
        ProposalResp r;
        {
            std::unique_lock<std::mutex> lk(proposal_mutex_);
            proposal_cv_.wait(
                lk, [&] { return proposal_response_ != ProposalResp::None ||
                                   cancel_.load(); });
            r = proposal_response_;
            proposal_response_ = ProposalResp::None;
        }
        {
            std::lock_guard<std::mutex> lk(data_mutex_);
            state_.proposal_active = false;
        }
        return r;
    };

    // Экспорт одобренной статьи (общий для списка и одиночной страницы).
    auto export_approved = [&](const ExtractedArticle& art,
                               const std::string& label) -> bool {
        Article a;
        a.url = art.url.empty() ? src.url : art.url;
        a.id = sha256_hex(a.url);
        a.source = host_of(src.url);
        a.fetched_at = iso8601_now();
        a.published_at = art.published_at;
        a.language = cfg.rewrite.language;
        a.status = TaskStatus::Extracting;
        set_status(a, "предпросмотр: одобрено («" +
                      (art.title.empty() ? a.url : art.title) + "»)");
        a.title_original = art.title;
        a.body_original = art.body;
        a.source_image = art.image;  // ExtractedArticle.image → Article.source_image
        a.content_hash = sha256_hex(a.title_original + "\n" + a.body_original);
        if (!export_article(cfg, a)) {
            set_status(a, "ошибка обработки: " +
                          (art.title.empty() ? a.url : art.title) + " — " + a.error);
            return false;
        }
        set_status(a, "статья (предпросмотр): " +
                      (a.title_original.empty() ? a.url : a.title_original));
        return true;
    };

    // --- Режим списка: страница — это категория/лента из нескольких новостей.
    // Каждая новость обрабатывается ОТДЕЛЬНО (свои заголовок/фото/дата), чтобы
    // модель не «склеивала» их в один дайджест. ---
    std::vector<ExtractedArticle> items = extract_page_items(html, src.url, src.extract);
    std::vector<ExtractedArticle> list_items;
    for (auto& it : items) {
        if (!it.url.empty()) list_items.push_back(std::move(it));
    }
    // Дедуп по URL (как в обычном обходе).
    {
        std::vector<ExtractedArticle> uniq;
        std::set<std::string> seen;
        for (auto& it : list_items) {
            const std::string u = strip_tracking_params(it.url);
            if (seen.insert(u).second) uniq.push_back(std::move(it));
        }
        list_items = std::move(uniq);
    }
    // Ограничение числа новостей с источника (если задано).
    if (cfg.max_items_per_source > 0 &&
        static_cast<int>(list_items.size()) > cfg.max_items_per_source) {
        list_items.resize(static_cast<std::size_t>(cfg.max_items_per_source));
    }

    if (list_items.size() >= 2) {
        const int total = static_cast<int>(list_items.size());
        log("предпросмотр: список из " + std::to_string(total) +
            " новостей — каждая обрабатывается отдельно");
        for (int i = 0; i < total; ++i) {
            if (cancel_.load()) {
                log("предпросмотр прерван пользователем");
                return false;
            }
            ExtractedArticle cand = list_items[i];
            // Подгружаем реальную страницу новости и извлекаем полный текст +
            // свою hero-картинку; при сбое — сниппет с листинга.
            ExtractedArticle full = cand;
            if (!cand.url.empty()) {
                const FetchResult ar = fetcher_->fetch(cand.url, "page", cfg.network);
                if (ar.ok) full = extract_page(ar.html, cand.url, src.extract);
            }
            full.url = strip_tracking_params(cand.url);
            full.image = full.image.empty() ? cand.image : full.image;
            if (full.title.empty()) full.title = cand.title;
            full.published_at = cand.published_at;

            const ProposalResp r = present(full, i + 1, total, "новость из списка");
            if (cancel_.load()) {
                log("предпросмотр прерван пользователем");
                return false;
            }
            // «Одобрить» — переписываем эту новость и идём к следующей; «Пересчитать»
            // — пропускаем. Прогон идёт по ВСЕМУ списку за один раз, в конце
            // завершается. Прервать в любой момент можно кнопкой «Остановить обход».
            if (r == ProposalResp::Approve) {
                if (!export_approved(full, "список")) return false;  // ошибка экспорта
            }
            // Reject (пересчёт) — просто переходим к следующей новости.
        }
        log("предпросмотр: список обработан («" + src.url + "»)");
        return true;  // источник обработан (даже если все новости отклонены)
    }

    // --- Одиночная страница: несколько вариантов извлечения одной статьи. ---
    std::vector<ExtractionProposal> cands =
        extract_page_candidates(html, src.url, src.extract);
    if (cands.empty()) {
        Article a;
        a.url = src.url;
        a.id = sha256_hex(src.url);
        a.source = host_of(src.url);
        a.fetched_at = iso8601_now();
        a.language = cfg.rewrite.language;
        a.status = TaskStatus::Error;
        a.error = "не удалось извлечь текст страницы";
        set_status(a, "ошибка обработки: " + src.url + " — " + a.error);
        return false;
    }

    // Ставим «обученную» для этого хоста стратегию первой — чтобы при
    // повторном предпросмотре сразу предложить одобренный ранее вариант.
    const std::string host = host_of(src.url);
    const auto lit = learned_strategy_.find(host);
    if (lit != learned_strategy_.end()) {
        for (std::size_t i = 0; i < cands.size(); ++i) {
            if (cands[i].strategy == lit->second) {
                std::swap(cands[0], cands[i]);
                break;
            }
        }
    }

    const int total = static_cast<int>(cands.size());
    int idx = 0;
    while (idx < total) {
        if (cancel_.load()) {
            log("предпросмотр прерван пользователем");
            return false;
        }
        const ExtractionProposal& p = cands[idx];
        const ProposalResp r = present(p.article, idx + 1, total, p.strategy_name);
        if (cancel_.load()) {
            log("предпросмотр прерван пользователем");
            return false;
        }
        if (r == ProposalResp::Approve) {
            learned_strategy_[host] = p.strategy;
            save_learned();
            ExtractedArticle art = p.article;
            art.url = src.url;
            if (!export_approved(art, "одиночная страница")) return false;
            return true;
        }
        // Reject (пересчёт) — следующий вариант извлечения.
        ++idx;
    }

    log("предпросмотр: подходящих вариантов не найдено («" + src.url + "»)");
    return false;
}

} // namespace news_rewriter
