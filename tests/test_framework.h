#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <cassert>
#include <sstream>

namespace test {

struct TestCase {
    std::string name;
    std::function<void()> func;
};

class TestRunner {
public:
    static TestRunner& instance() {
        static TestRunner runner;
        return runner;
    }

    void addTest(const std::string& name, std::function<void()> func) {
        tests_.push_back({name, func});
    }

    int run() {
        int passed = 0;
        int failed = 0;
        int total = tests_.size();

        std::cout << "\n========================================" << std::endl;
        std::cout << "Running " << total << " tests..." << std::endl;
        std::cout << "========================================\n" << std::endl;

        for (const auto& test : tests_) {
            std::cout << "[TEST] " << test.name << "... ";
            try {
                test.func();
                std::cout << "PASSED" << std::endl;
                passed++;
            } catch (const std::exception& e) {
                std::cout << "FAILED: " << e.what() << std::endl;
                failed++;
            } catch (...) {
                std::cout << "FAILED: unknown exception" << std::endl;
                failed++;
            }
        }

        std::cout << "\n========================================" << std::endl;
        std::cout << "Results: " << passed << " passed, " << failed << " failed, " << total << " total" << std::endl;
        std::cout << "========================================\n" << std::endl;

        return failed == 0 ? 0 : 1;
    }

private:
    std::vector<TestCase> tests_;
};

} // namespace test

#define TEST_ASSERT(expr) \
    do { \
        if (!(expr)) { \
            std::ostringstream oss; \
            oss << "Assertion failed: " #expr " at " __FILE__ ":" << __LINE__; \
            throw std::runtime_error(oss.str()); \
        } \
    } while(0)

#define TEST_ASSERT_EQUAL(a, b) \
    do { \
        if ((a) != (b)) { \
            std::ostringstream oss; \
            oss << "Assertion failed: " #a " == " #b " at " __FILE__ ":" << __LINE__; \
            throw std::runtime_error(oss.str()); \
        } \
    } while(0)

#define TEST_ASSERT_TRUE(expr) TEST_ASSERT(expr)
#define TEST_ASSERT_FALSE(expr) TEST_ASSERT(!(expr))

#define REGISTER_TEST(func) \
    static bool _registered_##func = []() { \
        test::TestRunner::instance().addTest(#func, func); \
        return true; \
    }()
