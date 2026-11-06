#include "file_lock.h"

#include <mutex>

namespace coder {
namespace file_lock {

namespace {

/* Полосы блокировок. Размер кратен 64 — по числу полос процессора
 * на типовой машине; точность здесь не нужна (см. file_lock.h). */
constexpr size_t kStripes = 64;
std::mutex g_stripes[kStripes];

size_t stripe_of(const std::string& path) {
    /* FNV-1a: путь может быть длинным, а нужен только индекс полосы. */
    size_t h = 1469598103934665603ull;
    for (char c : path) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    return h % kStripes;
}

} // anonymous namespace

Guard::Guard(const std::string& path) : slot_(&g_stripes[stripe_of(path)]) {
    static_cast<std::mutex*>(slot_)->lock();
}

Guard::~Guard() {
    static_cast<std::mutex*>(slot_)->unlock();
}

} // namespace file_lock
} // namespace coder
