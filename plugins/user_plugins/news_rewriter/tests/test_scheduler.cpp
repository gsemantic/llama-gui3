#include "test_framework.h"

#include <chrono>

#include "scheduler.h"

using namespace news_rewriter;

namespace {
using Clock = Scheduler::Clock;
using TimePoint = Scheduler::TimePoint;
} // namespace

static void test_scheduler_inactive_by_default() {
    Scheduler s;
    Config cfg = default_config();
    cfg.schedule_minutes = 0;
    s.configure(cfg);

    TEST_ASSERT_TRUE(!s.schedule_active());
    TEST_ASSERT_TRUE(s.next_delay() > std::chrono::seconds(0));
    TEST_ASSERT_TRUE(!s.due());
}

static void test_scheduler_timer_fires_after_interval() {
    TimePoint current = Clock::now();
    Scheduler s([&] { return current; });
    Config cfg = default_config();
    cfg.schedule_minutes = 5;
    s.configure(cfg);

    TEST_ASSERT_TRUE(s.schedule_active());
    TEST_ASSERT_TRUE(!s.due());

    current += std::chrono::minutes(4) + std::chrono::seconds(59);
    TEST_ASSERT_TRUE(!s.due());
    TEST_ASSERT_TRUE(s.next_delay() > std::chrono::seconds(0));

    current += std::chrono::seconds(1);   // ровно 5 минут
    TEST_ASSERT_TRUE(s.due());
    TEST_ASSERT_EQUAL(s.next_delay().count(), 0);
}

static void test_scheduler_run_resets_timer() {
    TimePoint current = Clock::now();
    Scheduler s([&] { return current; });
    Config cfg = default_config();
    cfg.schedule_minutes = 10;
    s.configure(cfg);

    current += std::chrono::minutes(10);
    TEST_ASSERT_TRUE(s.due());

    s.note_run_started();   // после запуска следующий — через интервал
    TEST_ASSERT_TRUE(!s.due());
    TEST_ASSERT_TRUE(s.next_delay() > std::chrono::seconds(0));
}

static void test_scheduler_interval_change_resets_timer() {
    TimePoint current = Clock::now();
    Scheduler s([&] { return current; });
    Config cfg = default_config();
    cfg.schedule_minutes = 5;
    s.configure(cfg);

    current += std::chrono::minutes(4);
    cfg.schedule_minutes = 10;   // интервал изменился → таймер сброшен
    s.configure(cfg);
    TEST_ASSERT_TRUE(!s.due());
    TEST_ASSERT_TRUE(s.next_delay() > std::chrono::minutes(9));
}

static void test_scheduler_disable_schedule() {
    TimePoint current = Clock::now();
    Scheduler s([&] { return current; });
    Config cfg = default_config();
    cfg.schedule_minutes = 5;
    s.configure(cfg);
    current += std::chrono::minutes(5);
    TEST_ASSERT_TRUE(s.due());

    cfg.schedule_minutes = 0;   // расписание выключено
    s.configure(cfg);
    TEST_ASSERT_TRUE(!s.schedule_active());
    TEST_ASSERT_TRUE(!s.due());
    TEST_ASSERT_TRUE(s.next_delay() > std::chrono::seconds(0));
}

static void test_scheduler_force_due() {
    Scheduler s;
    Config cfg = default_config();
    cfg.schedule_minutes = 30;
    s.configure(cfg);
    TEST_ASSERT_TRUE(!s.due());

    s.force_due();
    TEST_ASSERT_TRUE(s.due());
}

static void test_retry_policy_defaults() {
    Scheduler s;
    Config cfg = default_config();
    cfg.schedule_minutes = 0;
    s.configure(cfg);

    TEST_ASSERT_TRUE(s.can_retry(0));
    TEST_ASSERT_TRUE(s.can_retry(1));
    TEST_ASSERT_TRUE(s.can_retry(2));
    TEST_ASSERT_TRUE(!s.can_retry(3));   // после 3 ретраев — сдаёмся

    TEST_ASSERT_EQUAL(s.retry_delay(0).count(), 5);
    TEST_ASSERT_EQUAL(s.retry_delay(1).count(), 30);
    TEST_ASSERT_EQUAL(s.retry_delay(2).count(), 300);
    TEST_ASSERT_EQUAL(s.retry_delay(3).count(), 300);   // зажимается на максимуме
}

static void test_retry_policy_custom() {
    Scheduler s;
    Config cfg = default_config();
    cfg.schedule_minutes = 0;
    RetryPolicy rp;
    rp.max_retries = 1;
    rp.backoff_seconds = {0, 1};
    s.configure(cfg, rp);

    TEST_ASSERT_TRUE(s.can_retry(0));
    TEST_ASSERT_TRUE(!s.can_retry(1));
    TEST_ASSERT_EQUAL(s.retry_delay(0).count(), 0);
    TEST_ASSERT_EQUAL(s.retry_delay(1).count(), 1);
}

static void test_retry_no_backoff_values() {
    Scheduler s;
    Config cfg = default_config();
    cfg.schedule_minutes = 0;
    RetryPolicy rp;
    rp.max_retries = 5;
    rp.backoff_seconds.clear();
    s.configure(cfg, rp);

    TEST_ASSERT_TRUE(s.can_retry(4));
    TEST_ASSERT_EQUAL(s.retry_delay(0).count(), 0);
    TEST_ASSERT_EQUAL(s.retry_delay(7).count(), 0);
}

REGISTER_TEST(test_scheduler_inactive_by_default);
REGISTER_TEST(test_scheduler_timer_fires_after_interval);
REGISTER_TEST(test_scheduler_run_resets_timer);
REGISTER_TEST(test_scheduler_interval_change_resets_timer);
REGISTER_TEST(test_scheduler_disable_schedule);
REGISTER_TEST(test_scheduler_force_due);
REGISTER_TEST(test_retry_policy_defaults);
REGISTER_TEST(test_retry_policy_custom);
REGISTER_TEST(test_retry_no_backoff_values);
