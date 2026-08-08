#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "config.h"

namespace news_rewriter {

// Политика ретраев: сколько повторных попыток и задержки backoff (секунды).
// backoff_seconds[0] — перед 1-й повторной попыткой, [1] — перед 2-й и т.д.
// Индекс зажимается на последнем значении.
struct RetryPolicy {
    int max_retries = 3;                       // повторных попыток после 1-го сбоя
    std::vector<int> backoff_seconds{5, 30, 300};  // 5с → 30с → 5мин
};

// Планировщик: расписание авто-обхода и политика ретраев. Чистая логика без
// потоков и сети; используется worker-потоком и тестируется с инъекцией часов.
//
// Семантика ретраев: attempts_done = сколько повторных попыток уже сделано.
//   can_retry(attempts_done)  — ещё не исчерпаны (attempts_done < max_retries).
//   retry_delay(attempts_done) — пауза перед следующей (attempts_done+1)-й попыткой.
class Scheduler {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using NowFn = std::function<TimePoint()>;

    explicit Scheduler(NowFn now = nullptr);

    // Применяет интервал расписания и политику ретраев. При смене интервала
    // таймер сбрасывается (следующий запуск — через новый интервал).
    void configure(const Config& cfg, const RetryPolicy& retry = RetryPolicy());

    bool schedule_active() const;                  // schedule_minutes > 0
    std::chrono::seconds next_delay() const;       // до авто-запуска (0 = пора)
    bool due() const;                              // next_delay() == 0
    void note_run_started();                       // сброс таймера после запуска
    void force_due();                              // тесты/отладка: "пора сейчас"

    bool can_retry(uint32_t attempts_done) const;
    std::chrono::seconds retry_delay(uint32_t attempts_done) const;
    const RetryPolicy& retry_policy() const { return retry_; }

private:
    TimePoint now() const;

    int schedule_minutes_ = 0;                     // 0 = только ручной запуск
    RetryPolicy retry_;
    TimePoint next_run_{};
    bool have_next_ = false;
    NowFn now_fn_;
};

} // namespace news_rewriter
