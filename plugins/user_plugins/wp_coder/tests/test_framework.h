#pragma once

/*
 * test_framework.h — Минимальный тестовый фреймворк для AI-кодера.
 *
 * Использует attr(used) для предотвращения удаления статических регистраторов.
 *
 * Сторож на каждый тест (kWatchdogSeconds). Зависание теста в CI — это
 * загадка без номера: прогон просто стоит, и непонятно, где. Особенно
 * неприятно here, где половина кода работает с мьютексами и ожиданием
 * разрешения пользователя, то есть висеть там умеет. Сторож печатает имя
 * текущего теста и завершает процесс с особым кодом, после чего падение
 * становится диагнозом, а не наблюдением.
 */

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <functional>
#include <mutex>

struct TestCase {
    std::string name;
    std::function<void()> func;
};

inline std::vector<TestCase>& get_tests() {
    static std::vector<TestCase> tests;
    return tests;
}

struct TestRegistrar {
    TestRegistrar(const std::string& name, std::function<void()> func) {
        get_tests().push_back({name, std::move(func)});
    }
};

/* Макрос с атрибутом used предотвращает удаление линкером. */
#if defined(__GNUC__) || defined(__clang__)
    #define USED __attribute__((used))
#else
    #define USED
#endif

#define TEST(name) \
    static void test_##name(); \
    static USED TestRegistrar reg_##name(#name, test_##name); \
    static void test_##name()

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        std::cerr << "  FAIL: " << #a << " != " << #b << std::endl; \
        std::cerr << "    got: " << (a) << std::endl; \
        std::cerr << "    exp: " << (b) << std::endl; \
        throw std::runtime_error("ASSERT_EQ failed"); \
    } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        std::cerr << "  FAIL: " << #x << " is false" << std::endl; \
        throw std::runtime_error("ASSERT_TRUE failed"); \
    } \
} while(0)

#define ASSERT_FALSE(x) do { \
    if ((x)) { \
        std::cerr << "  FAIL: " << #x << " is true" << std::endl; \
        throw std::runtime_error("ASSERT_FALSE failed"); \
    } \
} while(0)

#define RUN_ALL_TESTS() \
    int main() { \
        constexpr int kWatchdogSeconds = 30; \
        std::atomic<int> token{0}; \
        std::atomic<bool> current_name_set{false}; \
        std::string current_name; \
        std::mutex current_name_mutex; \
        std::thread watchdog([&] { \
            int last = 0; \
            auto last_change = std::chrono::steady_clock::now(); \
            while (true) { \
                std::this_thread::sleep_for(std::chrono::milliseconds(250)); \
                const int now = token.load(); \
                if (now != last) { \
                    last = now; \
                    last_change = std::chrono::steady_clock::now(); \
                    continue; \
                } \
                const auto waited = std::chrono::duration_cast<std::chrono::seconds>( \
                    std::chrono::steady_clock::now() - last_change).count(); \
                if (waited >= kWatchdogSeconds) { \
                    std::cerr << "\nСТОРОЖ: тест " \
                              << (current_name_set.load() ? current_name \
                                                          : std::string("?")) \
                              << " идёт дольше " << kWatchdogSeconds \
                              << " с. Это дедлок или ожидание, которое никто" \
                                 " не снимет." << std::endl; \
                    std::_Exit(3); \
                } \
            } \
        }); \
        watchdog.detach(); \
        int passed = 0, failed = 0; \
        for (const auto& t : get_tests()) { \
            { \
                std::lock_guard<std::mutex> lk(current_name_mutex); \
                current_name = t.name; \
                current_name_set = true; \
            } \
            std::cout << "[TEST] " << t.name << "... " << std::flush; \
            token.fetch_add(1); \
            try { \
                t.func(); \
                std::cout << "OK" << std::endl; \
                passed++; \
            } catch (const std::exception& e) { \
                std::cout << "FAILED" << std::endl; \
                failed++; \
            } \
            token.fetch_add(1); \
        } \
        std::cout << "\n=== " << passed << " passed, " << failed \
                  << " failed ===" << std::endl; \
        return failed > 0 ? 1 : 0; \
    }
