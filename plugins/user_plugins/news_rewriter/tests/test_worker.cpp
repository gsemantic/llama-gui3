#include "test_framework.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

#include "worker.h"

using namespace news_rewriter;

namespace {

// Фейковый загрузчик: возвращает заранее заданные результаты без сети.
class FakeFetcher : public IFetch {
public:
    std::function<FetchResult(const std::string& url, const std::string& type)> impl;
    std::atomic<int> calls{0};

    bool init() override { return true; }
    bool is_available() const override { return true; }

    FetchResult fetch(const std::string& url, const std::string& type,
                      const NetworkConfig&) override {
        calls++;
        if (impl) return impl(url, type);
        FetchResult r;
        r.ok = true;
        return r;
    }
};

// Возвращает один элемент ленты с link = url.
FetchResult one_item(const std::string& url) {
    FetchResult r;
    r.ok = true;
    r.http_status = 200;
    FeedItem item;
    item.title = "Новость для " + url;
    item.link = url + "/news/1";
    item.description = "Текст новости";
    r.items.push_back(item);
    return r;
}

// Возвращает сырую HTML-страницу.
FetchResult one_page(const std::string& html) {
    FetchResult r;
    r.ok = true;
    r.http_status = 200;
    r.html = html;
    return r;
}

// Временный каталог для экспорта (уникальный, авто-очистка).
class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("news_rewriter_worker_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() { std::filesystem::remove_all(path_); }
    std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

// Регистрирует LocalFileSink и задаёт каталог данных воркеру.
void setup_worker_export(Worker& worker, const std::string& data_dir) {
    SinkRegistry::instance().register_factory("local_file", make_local_file_sink);
    worker.set_data_dir(data_dir);
}

} // namespace

static bool wait_until(const std::function<bool()>& cond, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return cond();
}

static Config make_test_config() {
    Config cfg = default_config();
    cfg.sources.clear();
    cfg.sources.push_back(SourceConfig{"https://a.example/rss", "rss", SourceExtract{}, true});
    cfg.sources.push_back(SourceConfig{"https://b.example/rss", "rss", SourceExtract{}, true});
    cfg.sources.push_back(SourceConfig{"https://c.example/rss", "rss", SourceExtract{}, false});
    return cfg;
}

static void test_worker_run_completes() {
    Worker worker;
    TempDir tmp;
    setup_worker_export(worker, tmp.path());
    worker.set_config(make_test_config());
    auto fetcher = std::make_unique<FakeFetcher>();
    FakeFetcher* f = fetcher.get();
    f->impl = [](const std::string& url, const std::string&) { return one_item(url); };
    worker.set_fetcher(std::move(fetcher));

    std::atomic<int> log_count{0};
    worker.set_log_callback([&](const std::string&) { log_count++; });

    TEST_ASSERT_TRUE(worker.start());
    worker.post(Command{CmdType::RunNow});

    const bool finished = wait_until([&] {
        WorkerState s = worker.snapshot();
        return s.running == false && s.last_message.find("обход завершён") != std::string::npos;
    });
    TEST_ASSERT_TRUE(finished);

    const WorkerState state = worker.snapshot();
    TEST_ASSERT_EQUAL(state.articles.size(), std::size_t(2));  // 2 включённых источника × 1 новость
    for (const auto& a : state.articles) {
        TEST_ASSERT_TRUE(a.status == TaskStatus::Done);
    }
    TEST_ASSERT_EQUAL(state.pending_tasks, 0);
    TEST_ASSERT_TRUE(state.last_run_unix > 0);
    TEST_ASSERT_TRUE(log_count.load() > 0);
    TEST_ASSERT_EQUAL(f->calls.load(), 2);

    worker.stop_and_join();
}

static void test_worker_config_reload() {
    Worker worker;
    worker.set_config(make_test_config());
    worker.set_fetcher(std::make_unique<FakeFetcher>());
    TEST_ASSERT_TRUE(worker.start());

    const std::string new_cfg = config_to_json(default_config()).dump();
    worker.post(Command{CmdType::ReloadConfig, new_cfg});

    const bool applied = wait_until([&] {
        const Config c = worker.get_config();
        return c.schedule_minutes == default_config().schedule_minutes &&
               c.sources.size() == default_config().sources.size();
    });
    TEST_ASSERT_TRUE(applied);

    worker.stop_and_join();
}

static void test_worker_does_not_run_disabled_sources() {
    Worker worker;
    worker.set_config(make_test_config());
    auto fetcher = std::make_unique<FakeFetcher>();
    FakeFetcher* f = fetcher.get();
    f->impl = [](const std::string& url, const std::string&) { return one_item(url); };
    worker.set_fetcher(std::move(fetcher));
    TEST_ASSERT_TRUE(worker.start());

    worker.post(Command{CmdType::RunNow});
    const bool finished = wait_until([&] {
        WorkerState s = worker.snapshot();
        return s.running == false && s.last_message.find("обход завершён") != std::string::npos;
    });
    TEST_ASSERT_TRUE(finished);
    TEST_ASSERT_EQUAL(worker.snapshot().articles.size(), std::size_t(2));  // 2 новости
    TEST_ASSERT_EQUAL(f->calls.load(), 2);  // отключённый источник не грузится

    worker.stop_and_join();
}

static void test_worker_extracts_rss_item_title() {
    Worker worker;
    TempDir tmp;
    setup_worker_export(worker, tmp.path());
    Config cfg = default_config();
    cfg.sources.clear();
    cfg.sources.push_back(SourceConfig{"https://a.example/rss", "rss", SourceExtract{}, true});
    worker.set_config(cfg);
    auto fetcher = std::make_unique<FakeFetcher>();
    fetcher->impl = [](const std::string&, const std::string&) {
        FetchResult r;
        r.ok = true;
        r.http_status = 200;
        FeedItem item;
        item.title = "<b>Важная &amp; новость</b>";
        item.link = "https://a.example/news/1";
        item.description = "<p>Описание с <script>var x=1;</script>тегом.</p>";
        r.items.push_back(item);
        return r;
    };
    worker.set_fetcher(std::move(fetcher));
    TEST_ASSERT_TRUE(worker.start());

    worker.post(Command{CmdType::RunNow});
    const bool finished = wait_until([&] {
        WorkerState s = worker.snapshot();
        return s.running == false && s.last_message.find("обход завершён") != std::string::npos;
    });
    TEST_ASSERT_TRUE(finished);

    const WorkerState state = worker.snapshot();
    TEST_ASSERT_EQUAL(state.articles.size(), std::size_t(1));
    // заголовок очищен от тегов и сущностей
    TEST_ASSERT_EQUAL(state.articles[0].title, "Важная & новость");

    worker.stop_and_join();
}

static void test_worker_extracts_page_title_and_body() {
    Worker worker;
    TempDir tmp;
    setup_worker_export(worker, tmp.path());
    Config cfg = default_config();
    cfg.sources.clear();
    cfg.sources.push_back(SourceConfig{"https://a.example/page", "page", SourceExtract{}, true});
    worker.set_config(cfg);
    auto fetcher = std::make_unique<FakeFetcher>();
    fetcher->impl = [](const std::string&, const std::string&) {
        return one_page(
            "<html><head><title>Сайт</title></head><body>"
            "<h1>Заголовок статьи</h1>"
            "<p>Первый абзац длинного текста новости.</p>"
            "<p>Второй абзац ещё длиннее, он и должен стать основным текстом "
            "при эвристике по самому длинному блоку.</p>"
            "</body></html>");
    };
    worker.set_fetcher(std::move(fetcher));
    TEST_ASSERT_TRUE(worker.start());

    worker.post(Command{CmdType::RunNow});
    const bool finished = wait_until([&] {
        WorkerState s = worker.snapshot();
        return s.running == false && s.last_message.find("обход завершён") != std::string::npos;
    });
    TEST_ASSERT_TRUE(finished);

    const WorkerState state = worker.snapshot();
    TEST_ASSERT_EQUAL(state.articles.size(), std::size_t(1));
    TEST_ASSERT_EQUAL(state.articles[0].title, "Заголовок статьи");

    worker.stop_and_join();
}

static void test_worker_llm_rewrites_articles() {
    Worker worker;
    TempDir tmp;
    setup_worker_export(worker, tmp.path());
    Config cfg = default_config();
    cfg.sources.clear();
    cfg.sources.push_back(SourceConfig{"https://a.example/rss", "rss", SourceExtract{}, true});
    worker.set_config(cfg);
    auto fetcher = std::make_unique<FakeFetcher>();
    fetcher->impl = [](const std::string&, const std::string&) { return one_item("https://a.example/rss"); };
    worker.set_fetcher(std::move(fetcher));

    std::atomic<int> llm_calls{0};
    worker.set_llm([&](const std::string& prompt, std::string& response, std::string&) -> bool {
        llm_calls++;
        TEST_ASSERT(prompt.find("{title}") == std::string::npos);  // подстановки применены
        response = "Новый заголовок\n\nНовый текст";
        return true;
    });
    TEST_ASSERT_TRUE(worker.start());

    worker.post(Command{CmdType::RunNow});
    const bool finished = wait_until([&] {
        WorkerState s = worker.snapshot();
        return s.running == false && s.last_message.find("обход завершён") != std::string::npos;
    });
    TEST_ASSERT_TRUE(finished);

    const WorkerState state = worker.snapshot();
    TEST_ASSERT_EQUAL(state.articles.size(), std::size_t(1));
    TEST_ASSERT_TRUE(state.articles[0].status == TaskStatus::Done);
    TEST_ASSERT_EQUAL(llm_calls.load(), 1);

    worker.stop_and_join();
}

static void test_worker_llm_error_marks_article() {
    Worker worker;
    Config cfg = default_config();
    cfg.sources.clear();
    cfg.sources.push_back(SourceConfig{"https://a.example/rss", "rss", SourceExtract{}, true});
    worker.set_config(cfg);
    auto fetcher = std::make_unique<FakeFetcher>();
    fetcher->impl = [](const std::string&, const std::string&) { return one_item("https://a.example/rss"); };
    worker.set_fetcher(std::move(fetcher));

    std::atomic<int> llm_calls{0};
    worker.set_llm([&](const std::string&, std::string&, std::string& error) -> bool {
        llm_calls++;
        error = "LLM не подключён";
        return false;
    });
    RetryPolicy rp;
    rp.max_retries = 2;                 // всего 3 попытки, как в чате
    rp.backoff_seconds = {0, 0};        // без пауз — тест быстрый
    worker.set_llm_retry_policy(rp);
    TEST_ASSERT_TRUE(worker.start());

    worker.post(Command{CmdType::RunNow});
    const bool finished = wait_until([&] {
        WorkerState s = worker.snapshot();
        return s.running == false && s.last_message.find("обход завершён") != std::string::npos;
    });
    TEST_ASSERT_TRUE(finished);

    const WorkerState state = worker.snapshot();
    TEST_ASSERT_EQUAL(state.articles.size(), std::size_t(1));
    TEST_ASSERT_TRUE(state.articles[0].status == TaskStatus::Error);
    TEST_ASSERT_EQUAL(state.articles[0].error, "LLM не подключён");
    TEST_ASSERT_EQUAL(state.articles[0].retry_count, 2);  // 3 попытки всего
    TEST_ASSERT_EQUAL(llm_calls.load(), 3);
    TEST_ASSERT_EQUAL(state.error_count, 1);
    TEST_ASSERT_EQUAL(state.done_count, 0);
    TEST_ASSERT_TRUE(state.last_message.find("с ошибками") != std::string::npos);

    worker.stop_and_join();
}

static void test_worker_llm_retries_then_succeeds() {
    Worker worker;
    TempDir tmp;
    setup_worker_export(worker, tmp.path());
    Config cfg = default_config();
    cfg.sources.clear();
    cfg.sources.push_back(SourceConfig{"https://a.example/rss", "rss", SourceExtract{}, true});
    worker.set_config(cfg);
    auto fetcher = std::make_unique<FakeFetcher>();
    fetcher->impl = [](const std::string&, const std::string&) { return one_item("https://a.example/rss"); };
    worker.set_fetcher(std::move(fetcher));

    std::atomic<int> llm_calls{0};
    worker.set_llm([&](const std::string&, std::string& response, std::string& error) -> bool {
        if (llm_calls.fetch_add(1) < 2) {   // 2 сбоя, затем успех
            error = "модель не ответила";
            return false;
        }
        response = "Заголовок после ретраев\n\nТекст";
        return true;
    });
    RetryPolicy rp;
    rp.max_retries = 2;
    rp.backoff_seconds = {0, 0};
    worker.set_llm_retry_policy(rp);
    TEST_ASSERT_TRUE(worker.start());

    worker.post(Command{CmdType::RunNow});
    const bool finished = wait_until([&] {
        WorkerState s = worker.snapshot();
        return s.running == false && s.last_message.find("обход завершён") != std::string::npos;
    });
    TEST_ASSERT_TRUE(finished);

    const WorkerState state = worker.snapshot();
    TEST_ASSERT_EQUAL(state.articles.size(), std::size_t(1));
    TEST_ASSERT_TRUE(state.articles[0].status == TaskStatus::Done);
    TEST_ASSERT_EQUAL(llm_calls.load(), 3);          // 2 неудачи + 1 успех
    TEST_ASSERT_EQUAL(state.done_count, 1);
    TEST_ASSERT_EQUAL(state.error_count, 0);

    worker.stop_and_join();
}

static void test_worker_llm_retries_exhausted_reports_error() {
    Worker worker;
    TempDir tmp;
    setup_worker_export(worker, tmp.path());
    Config cfg = default_config();
    cfg.sources.clear();
    cfg.sources.push_back(SourceConfig{"https://a.example/rss", "rss", SourceExtract{}, true});
    worker.set_config(cfg);
    auto fetcher = std::make_unique<FakeFetcher>();
    fetcher->impl = [](const std::string&, const std::string&) { return one_item("https://a.example/rss"); };
    worker.set_fetcher(std::move(fetcher));

    std::atomic<int> llm_calls{0};
    worker.set_llm([&](const std::string&, std::string&, std::string& error) -> bool {
        llm_calls++;
        error = "таймаут облачной модели";
        return false;
    });
    RetryPolicy rp;
    rp.max_retries = 2;
    rp.backoff_seconds = {0, 0};
    worker.set_llm_retry_policy(rp);
    TEST_ASSERT_TRUE(worker.start());

    worker.post(Command{CmdType::RunNow});
    const bool finished = wait_until([&] {
        WorkerState s = worker.snapshot();
        return s.running == false && s.last_message.find("обход завершён") != std::string::npos;
    });
    TEST_ASSERT_TRUE(finished);

    const WorkerState state = worker.snapshot();
    TEST_ASSERT_EQUAL(state.articles.size(), std::size_t(1));
    TEST_ASSERT_TRUE(state.articles[0].status == TaskStatus::Error);
    TEST_ASSERT_EQUAL(state.articles[0].error, "таймаут облачной модели");
    TEST_ASSERT_EQUAL(llm_calls.load(), 3);          // попытки исчерпаны
    TEST_ASSERT_EQUAL(state.error_count, 1);
    TEST_ASSERT_TRUE(state.last_message.find("с ошибками") != std::string::npos);

    worker.stop_and_join();
}

static void test_worker_summary_reports_no_errors() {
    Worker worker;
    TempDir tmp;
    setup_worker_export(worker, tmp.path());
    worker.set_config(make_test_config());
    auto fetcher = std::make_unique<FakeFetcher>();
    fetcher->impl = [](const std::string& url, const std::string&) { return one_item(url); };
    worker.set_fetcher(std::move(fetcher));
    TEST_ASSERT_TRUE(worker.start());

    worker.post(Command{CmdType::RunNow});
    const bool finished = wait_until([&] {
        WorkerState s = worker.snapshot();
        return s.running == false && s.last_message.find("обход завершён") != std::string::npos;
    });
    TEST_ASSERT_TRUE(finished);

    const WorkerState state = worker.snapshot();
    TEST_ASSERT_EQUAL(state.done_count, 2);          // 2 новости без LLM
    TEST_ASSERT_EQUAL(state.error_count, 0);
    TEST_ASSERT_TRUE(state.last_message.find("с ошибками") == std::string::npos);

    worker.stop_and_join();
}

static void test_worker_ignores_rerun_while_running() {
    Worker worker;
    worker.set_config(make_test_config());
    auto fetcher = std::make_unique<FakeFetcher>();
    fetcher->impl = [](const std::string& url, const std::string&) { return one_item(url); };
    worker.set_fetcher(std::move(fetcher));
    TEST_ASSERT_TRUE(worker.start());

    worker.post(Command{CmdType::RunNow});
    worker.post(Command{CmdType::RunNow});  // второй запрос не должен ломать очередь

    const bool finished = wait_until([&] {
        WorkerState s = worker.snapshot();
        return s.running == false && s.last_message.find("обход завершён") != std::string::npos;
    });
    TEST_ASSERT_TRUE(finished);

    worker.stop_and_join();
}

static void test_worker_exports_and_dedups() {
    Worker worker;
    TempDir tmp;
    setup_worker_export(worker, tmp.path());
    Config cfg = default_config();
    cfg.sources.clear();
    cfg.sources.push_back(SourceConfig{"https://a.example/rss", "rss", SourceExtract{}, true});
    worker.set_config(cfg);
    auto fetcher = std::make_unique<FakeFetcher>();
    fetcher->impl = [](const std::string&, const std::string&) { return one_item("https://a.example/rss"); };
    worker.set_fetcher(std::move(fetcher));
    TEST_ASSERT_TRUE(worker.start());

    // Первый обход: статья сохраняется на диск.
    worker.post(Command{CmdType::RunNow});
    TEST_ASSERT_TRUE(wait_until([&] {
        WorkerState s = worker.snapshot();
        return s.running == false && s.last_message.find("обход завершён") != std::string::npos;
    }));

    const std::filesystem::path root =
        std::filesystem::path(tmp.path()) / "news_rewriter" / "articles";
    TEST_ASSERT_TRUE(std::filesystem::exists(root));
    std::size_t file_count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) file_count++;
    }
    TEST_ASSERT(file_count >= 2);  // .json + .md

    // Второй обход: тот же URL — дубликат, файлы не добавляются.
    worker.post(Command{CmdType::RunNow});
    TEST_ASSERT_TRUE(wait_until([&] {
        WorkerState s = worker.snapshot();
        return s.running == false && s.last_message.find("обход завершён") != std::string::npos;
    }));

    std::size_t file_count2 = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) file_count2++;
    }
    TEST_ASSERT_EQUAL(file_count2, file_count);

    worker.stop_and_join();
}

static void test_worker_fetch_error_marks_article() {
    Worker worker;
    Config cfg = make_test_config();
    cfg.max_retries = 0;   // без ретраев — тест про маркировку ошибки
    worker.set_config(cfg);
    auto fetcher = std::make_unique<FakeFetcher>();
    fetcher->impl = [](const std::string&, const std::string&) {
        FetchResult r;
        r.ok = false;
        r.error = "сеть недоступна";
        return r;
    };
    worker.set_fetcher(std::move(fetcher));
    TEST_ASSERT_TRUE(worker.start());

    worker.post(Command{CmdType::RunNow});
    const bool finished = wait_until([&] {
        WorkerState s = worker.snapshot();
        return s.running == false && s.last_message.find("обход завершён") != std::string::npos;
    });
    TEST_ASSERT_TRUE(finished);

    const WorkerState state = worker.snapshot();
    TEST_ASSERT_EQUAL(state.articles.size(), std::size_t(2));  // по одной статье на источник
    for (const auto& a : state.articles) {
        TEST_ASSERT_TRUE(a.status == TaskStatus::Error);
        TEST_ASSERT_EQUAL(a.error, "сеть недоступна");
    }

    worker.stop_and_join();
}

static void test_worker_runs_scheduled_autonomously() {
    Worker worker;
    Config cfg = default_config();
    cfg.sources.clear();
    cfg.sources.push_back(SourceConfig{"https://a.example/rss", "rss", SourceExtract{}, true});
    cfg.schedule_minutes = 60;   // расписание включено
    worker.set_config(cfg);
    auto fetcher = std::make_unique<FakeFetcher>();
    fetcher->impl = [](const std::string& url, const std::string&) { return one_item(url); };
    worker.set_fetcher(std::move(fetcher));
    TEST_ASSERT_TRUE(worker.start());

    worker.debug_force_schedule_due();   // без команды RunNow — по расписанию
    const bool finished = wait_until([&] {
        WorkerState s = worker.snapshot();
        return s.running == false && s.last_message.find("обход завершён") != std::string::npos;
    });
    TEST_ASSERT_TRUE(finished);

    const WorkerState state = worker.snapshot();
    TEST_ASSERT_TRUE(state.scheduled);
    TEST_ASSERT_TRUE(state.next_run_in_seconds > 0);  // таймер сброшен после запуска
    TEST_ASSERT_EQUAL(state.articles.size(), std::size_t(1));

    worker.stop_and_join();
}

static void test_worker_retries_failing_source() {
    Worker worker;
    TempDir tmp;
    setup_worker_export(worker, tmp.path());
    Config cfg = default_config();
    cfg.sources.clear();
    cfg.sources.push_back(SourceConfig{"https://a.example/rss", "rss", SourceExtract{}, true});
    cfg.schedule_minutes = 0;   // без расписания
    cfg.max_retries = 2;        // количество попыток — из конфига
    worker.set_config(cfg);
    auto fetcher = std::make_unique<FakeFetcher>();
    FakeFetcher* f = fetcher.get();
    std::atomic<int> fail_until{2};   // первые 2 вызова — сеть недоступна
    f->impl = [&](const std::string& url, const std::string&) {
        if (fail_until.load() > 0) {
            fail_until--;
            FetchResult r;
            r.ok = false;
            r.error = "сеть недоступна";
            return r;
        }
        return one_item(url);
    };
    worker.set_fetcher(std::move(fetcher));
    RetryPolicy rp;
    rp.backoff_seconds = {0, 0, 0};   // без пауз — тест быстрый
    worker.set_retry_policy(rp);
    TEST_ASSERT_TRUE(worker.start());

    worker.post(Command{CmdType::RunNow});
    const bool finished = wait_until([&] {
        WorkerState s = worker.snapshot();
        return s.running == false && s.last_message.find("обход завершён") != std::string::npos;
    });
    TEST_ASSERT_TRUE(finished);

    // 2 неудачи + 1 успех = 3 вызова; в снапшоте — только статья (успешная).
    TEST_ASSERT_EQUAL(f->calls.load(), 3);
    const WorkerState state = worker.snapshot();
    TEST_ASSERT_EQUAL(state.articles.size(), std::size_t(1));
    TEST_ASSERT_TRUE(state.articles[0].status == TaskStatus::Done);

    worker.stop_and_join();
}

static void test_worker_retries_exhausted_marks_error() {
    Worker worker;
    Config cfg = default_config();
    cfg.sources.clear();
    cfg.sources.push_back(SourceConfig{"https://a.example/rss", "rss", SourceExtract{}, true});
    cfg.schedule_minutes = 0;
    cfg.max_retries = 2;
    worker.set_config(cfg);
    auto fetcher = std::make_unique<FakeFetcher>();
    FakeFetcher* f = fetcher.get();
    f->impl = [](const std::string&, const std::string&) {
        FetchResult r;
        r.ok = false;
        r.error = "сеть недоступна";
        return r;
    };
    worker.set_fetcher(std::move(fetcher));
    RetryPolicy rp;
    rp.backoff_seconds = {0, 0};
    worker.set_retry_policy(rp);
    TEST_ASSERT_TRUE(worker.start());

    worker.post(Command{CmdType::RunNow});
    const bool finished = wait_until([&] {
        WorkerState s = worker.snapshot();
        return s.running == false && s.last_message.find("обход завершён") != std::string::npos;
    });
    TEST_ASSERT_TRUE(finished);

    // 1 первичная + 2 ретрая = 3 вызова, статья Error с retry_count=2.
    TEST_ASSERT_EQUAL(f->calls.load(), 3);
    const WorkerState state = worker.snapshot();
    TEST_ASSERT_EQUAL(state.articles.size(), std::size_t(1));
    TEST_ASSERT_TRUE(state.articles[0].status == TaskStatus::Error);
    TEST_ASSERT_EQUAL(state.articles[0].retry_count, 2);

    worker.stop_and_join();
}

static void test_worker_stop_aborts_backoff() {
    Worker worker;
    Config cfg = default_config();
    cfg.sources.clear();
    cfg.sources.push_back(SourceConfig{"https://a.example/rss", "rss", SourceExtract{}, true});
    cfg.schedule_minutes = 0;
    cfg.max_retries = 10;
    worker.set_config(cfg);
    auto fetcher = std::make_unique<FakeFetcher>();
    FakeFetcher* f = fetcher.get();
    f->impl = [](const std::string&, const std::string&) {
        FetchResult r;
        r.ok = false;
        r.error = "сеть недоступна";
        return r;
    };
    worker.set_fetcher(std::move(fetcher));
    RetryPolicy rp;
    rp.backoff_seconds = {300};   // долгий backoff — проверяем прерывание
    worker.set_retry_policy(rp);
    TEST_ASSERT_TRUE(worker.start());

    worker.post(Command{CmdType::RunNow});
    TEST_ASSERT_TRUE(wait_until([&] { return f->calls.load() >= 1; }));  // вошёл в обход
    worker.post(Command{CmdType::Stop});

    const bool stopped = wait_until([&] {
        WorkerState s = worker.snapshot();
        return s.running == false;
    }, 2000);
    TEST_ASSERT_TRUE(stopped);   // backoff прерван, поток не завис

    worker.stop_and_join();
}

static void test_worker_no_fetcher_reports_error() {
    Worker worker;
    worker.set_config(make_test_config());
    worker.set_fetcher(nullptr);
    TEST_ASSERT_TRUE(worker.start());

    worker.post(Command{CmdType::RunNow});
    const bool finished = wait_until([&] {
        WorkerState s = worker.snapshot();
        return s.running == false && s.last_message.find("обход завершён") != std::string::npos;
    });
    TEST_ASSERT_TRUE(finished);

    const WorkerState state = worker.snapshot();
    TEST_ASSERT_EQUAL(state.articles.size(), std::size_t(2));
    for (const auto& a : state.articles) {
        TEST_ASSERT_TRUE(a.status == TaskStatus::Error);
    }

    worker.stop_and_join();
}

static void test_worker_filters_old_items() {
    Worker worker;
    TempDir tmp;
    setup_worker_export(worker, tmp.path());
    Config cfg = default_config();
    cfg.sources.clear();
    cfg.sources.push_back(SourceConfig{"https://a.example/rss", "rss", SourceExtract{}, true});
    cfg.max_age_hours = 24;   // свежесть: статья старше суток пропускается
    worker.set_config(cfg);
    auto fetcher = std::make_unique<FakeFetcher>();
    fetcher->impl = [](const std::string&, const std::string&) {
        FetchResult r;
        r.ok = true;
        r.http_status = 200;
        FeedItem old_item;
        old_item.title = "Старая новость";
        old_item.link = "https://a.example/news/old";
        old_item.description = "Текст";
        old_item.pub_date = "Sat, 01 Aug 2026 00:00:00 +0000";   // 7 дней назад
        FeedItem fresh_item;
        fresh_item.title = "Свежая новость";
        fresh_item.link = "https://a.example/news/fresh";
        fresh_item.description = "Текст";
        fresh_item.pub_date = iso8601_now();   // сейчас — заведомо свежая
        r.items.push_back(old_item);
        r.items.push_back(fresh_item);
        return r;
    };
    worker.set_fetcher(std::move(fetcher));
    TEST_ASSERT_TRUE(worker.start());

    worker.post(Command{CmdType::RunNow});
    const bool finished = wait_until([&] {
        WorkerState s = worker.snapshot();
        return s.running == false && s.last_message.find("обход завершён") != std::string::npos;
    });
    TEST_ASSERT_TRUE(finished);

    const WorkerState state = worker.snapshot();
    TEST_ASSERT_EQUAL(state.articles.size(), std::size_t(1));
    TEST_ASSERT_EQUAL(state.articles[0].title, "Свежая новость");

    worker.stop_and_join();
}

static void test_worker_limits_items_per_source() {
    Worker worker;
    TempDir tmp;
    setup_worker_export(worker, tmp.path());
    Config cfg = default_config();
    cfg.sources.clear();
    cfg.sources.push_back(SourceConfig{"https://a.example/rss", "rss", SourceExtract{}, true});
    cfg.max_items_per_source = 2;   // берём не более 2 новостей с источника
    worker.set_config(cfg);
    auto fetcher = std::make_unique<FakeFetcher>();
    fetcher->impl = [](const std::string&, const std::string&) {
        FetchResult r;
        r.ok = true;
        r.http_status = 200;
        for (int i = 1; i <= 5; i++) {
            FeedItem item;
            item.title = "Новость " + std::to_string(i);
            item.link = "https://a.example/news/" + std::to_string(i);
            item.description = "Текст";
            r.items.push_back(item);
        }
        return r;
    };
    worker.set_fetcher(std::move(fetcher));
    TEST_ASSERT_TRUE(worker.start());

    worker.post(Command{CmdType::RunNow});
    const bool finished = wait_until([&] {
        WorkerState s = worker.snapshot();
        return s.running == false && s.last_message.find("обход завершён") != std::string::npos;
    });
    TEST_ASSERT_TRUE(finished);

    const WorkerState state = worker.snapshot();
    TEST_ASSERT_EQUAL(state.articles.size(), std::size_t(2));

    worker.stop_and_join();
}

static void test_worker_uses_config_max_retries() {
    Worker worker;
    Config cfg = default_config();
    cfg.sources.clear();
    cfg.sources.push_back(SourceConfig{"https://a.example/rss", "rss", SourceExtract{}, true});
    cfg.schedule_minutes = 0;
    cfg.max_retries = 2;   // количество попыток — из конфига (без set_retry_policy max_retries)
    worker.set_config(cfg);
    auto fetcher = std::make_unique<FakeFetcher>();
    FakeFetcher* f = fetcher.get();
    f->impl = [](const std::string&, const std::string&) {
        FetchResult r;
        r.ok = false;
        r.error = "сеть недоступна";
        return r;
    };
    worker.set_fetcher(std::move(fetcher));
    RetryPolicy rp;
    rp.backoff_seconds = {0, 0};   // только паузы; max_retries берётся из конфига
    worker.set_retry_policy(rp);
    TEST_ASSERT_TRUE(worker.start());

    worker.post(Command{CmdType::RunNow});
    const bool finished = wait_until([&] {
        WorkerState s = worker.snapshot();
        return s.running == false && s.last_message.find("обход завершён") != std::string::npos;
    });
    TEST_ASSERT_TRUE(finished);

    // 1 первичная + 2 ретрая = 3 вызова; статья Error с retry_count=2.
    TEST_ASSERT_EQUAL(f->calls.load(), 3);
    const WorkerState state = worker.snapshot();
    TEST_ASSERT_EQUAL(state.articles.size(), std::size_t(1));
    TEST_ASSERT_TRUE(state.articles[0].status == TaskStatus::Error);
    TEST_ASSERT_EQUAL(state.articles[0].retry_count, 2);

    worker.stop_and_join();
}

REGISTER_TEST(test_worker_run_completes);
REGISTER_TEST(test_worker_extracts_rss_item_title);
REGISTER_TEST(test_worker_extracts_page_title_and_body);
REGISTER_TEST(test_worker_llm_rewrites_articles);
REGISTER_TEST(test_worker_llm_error_marks_article);
REGISTER_TEST(test_worker_llm_retries_then_succeeds);
REGISTER_TEST(test_worker_llm_retries_exhausted_reports_error);
REGISTER_TEST(test_worker_summary_reports_no_errors);
REGISTER_TEST(test_worker_exports_and_dedups);
REGISTER_TEST(test_worker_config_reload);
REGISTER_TEST(test_worker_does_not_run_disabled_sources);
REGISTER_TEST(test_worker_ignores_rerun_while_running);
REGISTER_TEST(test_worker_fetch_error_marks_article);
REGISTER_TEST(test_worker_no_fetcher_reports_error);
REGISTER_TEST(test_worker_runs_scheduled_autonomously);
REGISTER_TEST(test_worker_retries_failing_source);
REGISTER_TEST(test_worker_retries_exhausted_marks_error);
REGISTER_TEST(test_worker_stop_aborts_backoff);
REGISTER_TEST(test_worker_filters_old_items);
REGISTER_TEST(test_worker_limits_items_per_source);
REGISTER_TEST(test_worker_uses_config_max_retries);
