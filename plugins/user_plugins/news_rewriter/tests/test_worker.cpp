#include "test_framework.h"

#include <atomic>
#include <chrono>
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

    worker.set_llm([](const std::string&, std::string&, std::string& error) -> bool {
        error = "LLM не подключён";
        return false;
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
    TEST_ASSERT_TRUE(state.articles[0].status == TaskStatus::Error);
    TEST_ASSERT_EQUAL(state.articles[0].error, "LLM не подключён");

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

static void test_worker_fetch_error_marks_article() {
    Worker worker;
    Config cfg = make_test_config();
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

REGISTER_TEST(test_worker_run_completes);
REGISTER_TEST(test_worker_extracts_rss_item_title);
REGISTER_TEST(test_worker_extracts_page_title_and_body);
REGISTER_TEST(test_worker_llm_rewrites_articles);
REGISTER_TEST(test_worker_llm_error_marks_article);
REGISTER_TEST(test_worker_config_reload);
REGISTER_TEST(test_worker_does_not_run_disabled_sources);
REGISTER_TEST(test_worker_ignores_rerun_while_running);
REGISTER_TEST(test_worker_fetch_error_marks_article);
REGISTER_TEST(test_worker_no_fetcher_reports_error);
