#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "common.h"
#include "config.h"
#include "fetcher.h"
#include "rewriter.h"
#include "scheduler.h"
#include "sink.h"
#include "storage.h"

namespace news_rewriter {

// Команды, отправляемые UI в рабочий поток.
enum class CmdType {
    RunNow,          // обойти все включённые источники
    Stop,            // прервать текущий обход
    ReloadConfig,    // arg = JSON-конфигурация (обновить снапшот)
    DebugForceDue    // тесты/отладка: сделать авто-запуск "пора сейчас"
};

struct Command {
    Command() = default;
    Command(CmdType t, std::string a = {}) : type(t), arg(std::move(a)) {}

    CmdType type = CmdType::RunNow;
    std::string arg;
};

// Компактное состояние задачи для UI (без больших текстов).
struct ArticleStatusView {
    std::string url;
    std::string source;
    std::string title;              // извлечённый заголовок (title_original)
    TaskStatus status = TaskStatus::Pending;
    std::string error;
    uint32_t retry_count = 0;
};

// Снимок состояния воркера, читаемый UI каждый кадр.
struct WorkerState {
    bool worker_active = false;   // поток запущен
    bool running = false;         // идёт обход
    bool scheduled = false;       // расписание включено (schedule_minutes > 0)
    int next_run_in_seconds = -1; // до авто-запуска (-1 = расписание выключено)
    int pending_tasks = 0;
    std::uint64_t last_run_unix = 0;
    std::string last_message;
    int done_count = 0;           // статей обработано успешно (последний обход)
    int error_count = 0;          // статей с ошибкой (последний обход)
    int seo_missing = 0;          // опубликовано без SEO-мета (модель не ответила на SEO)
    std::vector<ArticleStatusView> articles;
};

// Рабочий поток: очередь команд + конвейер fetch→extract→rewrite→sink.
// ImGui и host-настройки вызываются ТОЛЬКО в main-потоке; worker работает
// с собственными снапшотами конфига и состояния.
class Worker {
public:
    using LogFn = std::function<void(const std::string&)>;

    Worker();
    ~Worker();

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    bool start();
    void post(Command cmd);
    void set_config(const Config& cfg);   // main-поток: обновить снапшот
    Config get_config() const;
    WorkerState snapshot() const;
    void set_log_callback(LogFn cb);      // main-поток: перенаправление в host log
    void set_fetcher(std::unique_ptr<IFetch> fetcher);  // тесты: подмена загрузчика
    void set_llm(LlmFn llm);              // main-поток: рерайт (только worker)
    void set_data_dir(const std::string& data_dir);  // main: корень Storage
    void set_retry_policy(const RetryPolicy& retry);  // тесты: ретраи без пауз
    void set_llm_retry_policy(const RetryPolicy& retry);  // тесты: ретраи LLM без пауз
    void debug_force_schedule_due();      // тесты: авто-запуск немедленно
    void stop_and_join();

private:
    void loop();
    void process_run(const Config& cfg);
    // Возвращает true, если источник обработан до конца (или сбой постоянный);
    // false — повторяемый сбой (сеть/таймаут), caller выполняет retry.
    bool process_source(const Config& cfg, const SourceConfig& src, uint32_t retries);
    // Рерайт статьи через LLM с ретраями (как в чате: до 3 попыток). Возвращает
    // false, если все попытки исчерпаны (a.error — текст последней ошибки).
    bool rewrite(Article& a, const Config& cfg);
    bool export_article(const Config& cfg, Article& a);  // рерайт + Sink
    bool sleep_interruptible(const std::chrono::seconds& delay);
    void remove_stale_source_error(const std::string& url);
    void log(const std::string& msg);
    void set_status(const Article& a, const std::string& msg);

    std::thread thread_;
    mutable std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::queue<Command> queue_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<bool> thread_done_{false};

    mutable std::mutex data_mutex_;
    Config config_;
    WorkerState state_;
    std::vector<Article> pipeline_;       // полные данные конвейера
    LogFn log_callback_;
    std::unique_ptr<IFetch> fetcher_;
    LlmFn llm_;
    RetryPolicy retry_policy_;            // только backoff-задержки (max_retries — из конфига)
    RetryPolicy llm_retry_policy_;        // ретраи рерайта: 1 + max_retries попыток, backoff
    std::string data_dir_;                // корень Storage (из path_data_dir хоста)
    Storage storage_;
    Scheduler scheduler_;                 // только worker-поток
    std::atomic<int> run_seo_missing_{0}; // статей без SEO в текущем обходе
    // SEO отключён на остаток обхода из-за rate-limit (чтобы не долбить
    // облако повторными SEO-вызовами и не расходовать квоту).
    std::atomic<bool> seo_skipped_{false};
};

} // namespace news_rewriter
