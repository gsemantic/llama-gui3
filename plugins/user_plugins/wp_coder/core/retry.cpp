/*
 * retry.cpp — тело политики повторов (И12.6).
 * Обоснование — в retry.h; здесь только код.
 */

#include "retry.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace coder {
namespace {

/* Расчётная задержка БЕЗ джиттера: initial * factor^n.
 *
 * Считается в double, а не сдвигом целого: factor из задачи — 2.0, но он
 * задан полем, и пользовательская настройка «1.5» при типе int молча дала бы
 * factor=1 и никакого роста. */
double base_delay_ms(const RetryPolicy& p, int attempt) {
    const double n = static_cast<double>(std::max(0, attempt));
    return static_cast<double>(p.initial_ms) * std::pow(p.factor, n);
}

/* Обрезка по потолку. */
int clamp_to_max(int delay_ms, int max_delay_ms) {
    if (delay_ms < 0) return 0;
    return delay_ms > max_delay_ms ? max_delay_ms : delay_ms;
}

} // namespace

int retry_delay_ms(const RetryPolicy& policy, int attempt, double roll,
                   int server_suggested_ms) {
    /* Сервер назвал своё время — оно и есть ответ, и джиттер к нему НЕ
     * применяется: «приходи через 8 с» и «приходи через 10 с» — это разные
     * обещания, и размывать второе на ±25 % значит нарушить первое. */
    if (server_suggested_ms >= 0) {
        return clamp_to_max(server_suggested_ms, policy.max_delay_ms);
    }

    const double base = base_delay_ms(policy, attempt);
    /* roll из [0, 1): 0 → нижняя граница, 1 → верхняя. Границы включены
     * ровно на 75 % и 125 %, то есть джиттер заявлен и проверяем. */
    const double r = std::min(1.0, std::max(0.0, roll));
    const double scaled = base * (1.0 - policy.jitter + 2.0 * policy.jitter * r);
    return clamp_to_max(static_cast<int>(scaled + 0.5), policy.max_delay_ms);
}

int retry_delay_ms_random(const RetryPolicy& policy, int attempt,
                          int server_suggested_ms) {
    static std::mt19937 gen(std::random_device{}());
    std::uniform_real_distribution<double> roll(0.0, 1.0);
    return retry_delay_ms(policy, attempt, roll(gen), server_suggested_ms);
}

bool should_retry(const RetryPolicy& policy, bool retryable, int attempt) {
    if (!retryable) return false;
    /* attempt — счётчик сделанных попыток с нуля, и решение «ещё раз»
     * принимается, пока НЕ исчерпан лимит ПОПЫТОК, а не повторов. */
    return attempt + 1 < policy.max_attempts;
}

std::string retry_notice(const std::string& error, FailureKind kind, int attempt,
                         int max_attempts, int delay_ms) {
    std::string out = "[повтор] ";
    out += failure_kind_name(kind);
    out += ": ";
    out += error.empty() ? std::string("причина не названа") : error;
    out += " — попытка " + std::to_string(attempt + 2) + "/" +
           std::to_string(max_attempts);
    /* Считаем секундами с округлением ВВЕРХ: «через 0 с» после ожидания
     * 400 мс читалось бы как «повтор немедленно», а человек, у которого
     * лента обновится через секунду, увидел бы расхождение. */
    const int seconds = (delay_ms + 999) / 1000;
    out += ", через " + std::to_string(seconds) + " с";
    return out;
}

} // namespace coder