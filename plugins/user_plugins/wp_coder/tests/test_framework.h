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
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#include <functional>
#include <mutex>

/* --- Уборка временных каталогов, созданных тестами ---
 *
 * Хелперы вида `make_tmp_tree()` снимают мусор ПЕРЕД созданием и не снимают
 * ПОСЛЕ: имя каталога включает pid, а pid у каждого прогона свой, то есть
 * снимать нечего, и каждый прогон оставлял свой набор каталогов. За сессию
 * это набегало сотнями каталогов на десятки мегабайт — в /tmp, где их и
 * видно только потому, что они никуда не делись.
 *
 * Уборка идёт ОДИН раз на прогон, в конце, по списку зарегистрированных
 * путей. Перебирать каталоги по префиксу имени нельзя: под теми же
 * именами работает живой плагин (сессии субагентов, каталоги снимков), и
 * `remove_all` по маске снёс бы чужое. Регистрация добровольная: фикстуры,
 * которые убирают каталог сами (RAII-деструктор), ничего регистрировать не
 * должны — иначе уборка удаляла бы уже удалённое. */
inline std::vector<std::string>& tmp_trees() {
    static std::vector<std::string> paths;
    return paths;
}

inline void register_tmp_tree(const std::string& path) {
    tmp_trees().push_back(path);
}

inline void cleanup_tmp_trees() {
    for (const std::string& p : tmp_trees()) {
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
        /* Ошибка уборки МОЛЧИТ: это не проверка, а уборка мусора, и падать
         * из-за неё после зелёного прогона значило бы превратить её в
         * проверку, которой она не является. Мусор, который не удалился,
         * виден в /tmp сам. */
    }
    tmp_trees().clear();
}

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
                /* Текст исключения печатается ВСЕГДА, даже если его не \
                 * было в ASSERT: падение из std::map::at или из разбора \
                 * JSON не печатает ничего само, и такой прогон выглядит \
                 * как «тест упал молча» — а это ровно тот случай, где \
                 * неверят красному и ищут причину вслепую. Исключение \
                 * поймано в любом случае, печать — не причина его \
                 * пропустить. */ \
                std::cerr << "  ИСКЛЮЧЕНИЕ: " << e.what() << std::endl; \
                std::cout << "FAILED" << std::endl; \
                failed++; \
            } \
            token.fetch_add(1); \
        } \
        std::cout << "\n=== " << passed << " passed, " << failed \
                  << " failed ===" << std::endl; \
        cleanup_tmp_trees(); \
        return failed > 0 ? 1 : 0; \
    }
