#pragma once

/*
 * test_framework.h — Минимальный тестовый фреймворк для AI-кодера.
 *
 * Использует attr(used) для предотвращения удаления статических регистраторов.
 */

#include <iostream>
#include <string>
#include <vector>
#include <functional>

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
        int passed = 0, failed = 0; \
        for (const auto& t : get_tests()) { \
            std::cout << "[TEST] " << t.name << "... "; \
            try { \
                t.func(); \
                std::cout << "OK" << std::endl; \
                passed++; \
            } catch (const std::exception& e) { \
                std::cout << "FAILED" << std::endl; \
                failed++; \
            } \
        } \
        std::cout << "\n=== " << passed << " passed, " << failed << " failed ===" << std::endl; \
        return failed > 0 ? 1 : 0; \
    }
