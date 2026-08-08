#pragma once

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <functional>

// Мини-фреймворк юнит-тестов плагина (не зависит от приложения).
namespace test {

struct TestCase {
    std::string name;
    std::function<void()> func;
};

class Runner {
public:
    static Runner& instance() {
        static Runner runner;
        return runner;
    }

    void add(const std::string& name, std::function<void()> func) {
        cases_.push_back({name, std::move(func)});
    }

    int run() {
        int passed = 0;
        int failed = 0;
        for (const auto& c : cases_) {
            std::cout << "[TEST] " << c.name << "... ";
            try {
                c.func();
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
        std::cout << "\nResults: " << passed << " passed, " << failed
                  << " failed, " << cases_.size() << " total" << std::endl;
        return failed == 0 ? 0 : 1;
    }

private:
    std::vector<TestCase> cases_;
};

} // namespace test

#define TEST_ASSERT(expr) \
    do { \
        if (!(expr)) { \
            std::ostringstream oss; \
            oss << "Assertion failed: " #expr " at " __FILE__ ":" << __LINE__; \
            throw std::runtime_error(oss.str()); \
        } \
    } while (0)

#define TEST_ASSERT_EQUAL(a, b) \
    do { \
        if ((a) != (b)) { \
            std::ostringstream oss; \
            oss << "Assertion failed: " #a " == " #b " at " __FILE__ ":" << __LINE__; \
            throw std::runtime_error(oss.str()); \
        } \
    } while (0)

#define TEST_ASSERT_TRUE(expr) TEST_ASSERT(expr)
#define TEST_ASSERT_FALSE(expr) TEST_ASSERT(!(expr))

#define REGISTER_TEST(func) \
    static bool _registered_##func = []() { \
        test::Runner::instance().add(#func, func); \
        return true; \
    }()
