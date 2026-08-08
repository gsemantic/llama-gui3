#include "scheduler.h"

#include <algorithm>

namespace news_rewriter {

Scheduler::Scheduler(NowFn now) : now_fn_(std::move(now)) {}

Scheduler::TimePoint Scheduler::now() const {
    return now_fn_ ? now_fn_() : Clock::now();
}

void Scheduler::configure(const Config& cfg, const RetryPolicy& retry) {
    retry_ = retry;
    const int minutes = cfg.schedule_minutes < 0 ? 0 : cfg.schedule_minutes;
    if (minutes == schedule_minutes_) return;   // интервал не изменился
    schedule_minutes_ = minutes;
    if (schedule_minutes_ > 0) {
        next_run_ = now() + std::chrono::minutes(schedule_minutes_);
        have_next_ = true;
    } else {
        have_next_ = false;
    }
}

bool Scheduler::schedule_active() const {
    return schedule_minutes_ > 0;
}

std::chrono::seconds Scheduler::next_delay() const {
    if (!have_next_) return std::chrono::hours(24);   // ждём только команды
    const TimePoint t = now();
    if (t >= next_run_) return std::chrono::seconds(0);
    return std::chrono::duration_cast<std::chrono::seconds>(next_run_ - t);
}

bool Scheduler::due() const {
    return next_delay() == std::chrono::seconds(0);
}

void Scheduler::note_run_started() {
    if (schedule_minutes_ <= 0) {
        have_next_ = false;
        return;
    }
    next_run_ = now() + std::chrono::minutes(schedule_minutes_);
    have_next_ = true;
}

void Scheduler::force_due() {
    if (schedule_minutes_ <= 0) return;
    next_run_ = now();
    have_next_ = true;
}

bool Scheduler::can_retry(uint32_t attempts_done) const {
    return attempts_done < static_cast<uint32_t>(std::max(0, retry_.max_retries));
}

std::chrono::seconds Scheduler::retry_delay(uint32_t attempts_done) const {
    const std::vector<int>& b = retry_.backoff_seconds;
    if (b.empty()) return std::chrono::seconds(0);
    std::size_t idx = static_cast<std::size_t>(attempts_done);
    if (idx >= b.size()) idx = b.size() - 1;
    return std::chrono::seconds(b[idx]);
}

} // namespace news_rewriter
