// id_prefix.cpp — идентификаторы сообщений, частей и сессий (И5.5).

#include "id_prefix.h"

#include <cstdio>

namespace coder {
namespace {

/* Дописать число нулями до kIdDigits. Ручная сборка, а не snprintf с
 * "%012llu": идентификаторы выдаются на каждый вызов инструмента, и
 * snprintf здесь был бы заметной частью стоимости вызова. */
std::string with_padding(unsigned long long value) {
    char buf[kIdDigits + 1];
    for (size_t i = 0; i < kIdDigits; ++i) {
        buf[kIdDigits - 1 - i] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    buf[kIdDigits] = '\0';
    return std::string(buf, kIdDigits);
}

} // namespace

std::string IdSequence::next(const char* prefix) {
    const unsigned long long value =
        next_.fetch_add(1, std::memory_order_relaxed) + 1;
    return std::string(prefix) + with_padding(value);
}

bool id_has_prefix(const std::string& id, const char* prefix) {
    const std::string p(prefix);
    if (id.size() != p.size() + kIdDigits) return false;
    return id.compare(0, p.size(), p) == 0;
}

unsigned long long id_number(const std::string& id) {
    const size_t dash = id.find('_');
    if (dash == std::string::npos || dash + 1 + kIdDigits != id.size()) {
        return 0;
    }
    unsigned long long value = 0;
    for (size_t i = dash + 1; i < id.size(); ++i) {
        const char c = id[i];
        if (c < '0' || c > '9') return 0;
        value = value * 10 + static_cast<unsigned long long>(c - '0');
    }
    return value;
}

void IdSequence::observe(const std::string& id) {
    const unsigned long long n = id_number(id);
    if (n == 0) return;
    /* Поднять так, чтобы следующий был строго больше: compare-exchange
     * вместо простого «если меньше — присвоить», иначе два потока,
     * одновременно открывающие сессию, выдали бы один и тот же id. */
    unsigned long long current = next_.load(std::memory_order_relaxed);
    while (current < n &&
           !next_.compare_exchange_weak(current, n,
                                        std::memory_order_relaxed)) {
        /* current обновлён compare_exchange, следующая итерация читает
         * уже новое значение. */
    }
}

} // namespace coder
