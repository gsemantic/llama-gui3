/*
 * test_file_lock.cpp — И4.5: сериализация правки файла и сохранение
 * формата (BOM/CRLF).
 *
 * Проверяется не «мьютекс существует», а два конкретных свойства:
 *
 *   1. Две правки одного файла из двух потоков не могут перемешаться.
 *      Это не гипотетика: Engine::pending_apply пишет файл из UI-потока
 *      по кнопке, а инструмент — из worker-потока агента, и обе правки
 *      цикл «прочитал → изменил → записал». Проверка построена так, что
 *      БЕЗ блокировки падает: каждый поток читает, ждёт, пока второй
 *      тоже прочитает, и оба пишут — теряется ровно одна правка.
 *
 *   2. Перезапись не меняет формат файла. Иначе правка двух строк
 *      выглядит в git как изменение всего файла, а .bat/.sh и
 *      Windows-редакторы перестают видеть файл как есть.
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/file_lock.h"
#include "../core/text_edit.h"
#include "../core/json.h"
#include "../core/tools_registry.h"
#include "../core/engine.h"
#include "../core/base_tools.h"
#include "../core/project.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <iterator>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace coder;

namespace {

fs::path make_tmp_tree() {
    fs::path tmp = fs::temp_directory_path()
        / ("wp_coder_lock_" + std::to_string(::getpid()) + "_"
           + std::to_string(std::rand()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    /* И11.5: каталог убирается в конце прогона (test_framework.h), а не остаётся в /tmp до следующего. */
    register_tmp_tree(tmp.string());
    return tmp;
}

void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

void init_tools(const fs::path& project) {
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&, LlmReply&) { return false; };
    cb.llm_complete = [](const std::string&, const std::string&, std::string&) { return false; };
    cb.llm_is_connected = []() { return false; };
    cb.chat_event = [](const std::string&) {};
    Engine::instance().init(cb);
    test_support::approve_all_permissions();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = project.string();
        engine_state().allowed_external_paths.clear();
        engine_state().plan_mode = false;
        engine_state().pending.clear();
    }
    register_base_tools();
}

std::string run_write(const std::string& path, const std::string& content) {
    json::JsonValue args = json::JsonValue::object();
    args.set("path", path);
    args.set("content", content);
    return ToolsRegistry::instance().run("write_file", args);
}

} // anonymous namespace

/* ======================================================================
 * Сериализация
 * ====================================================================== */

TEST(file_lock_serializes_two_threads_on_the_same_file) {
    fs::path tmp = make_tmp_tree();
    const std::string file = (tmp / "shared.txt").string();
    write_file(tmp / "shared.txt", "start\n");

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::string results[2];

    /* Оба потока делают ровно то, что делает инструмент: читают, потом
     * пишут. Синхронизация через «оба прочитали» сделана так, чтобы без
     * семафора гонка была гарантированной, а не вероятной. */
    auto worker = [&](int idx, const std::string& line) {
        std::string content;
        {
            /* Ждём, пока второй поток тоже дойдёт до чтения — иначе гонка
             * может не проявиться, и тест был бы проверкой удачи. */
            if (idx == 0) {
                ready = 1;
                while (!go.load()) std::this_thread::yield();
            } else {
                while (ready.load() == 0) std::this_thread::yield();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                go = true;
            }
            file_lock::Guard guard(file);
            std::ifstream f(file, std::ios::binary);
            content.assign((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            content += line;
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << content;
        }
        results[idx] = content;
    };

    std::thread t1(worker, 0, "one\n");
    std::thread t2(worker, 1, "two\n");
    t1.join();
    t2.join();

    /* Обе правки на месте: значит чтение и запись были разнесены по
     * семафору, а не наложились. */
    const std::string final_text = read_file(tmp / "shared.txt");
    ASSERT_TRUE(final_text.find("one") != std::string::npos);
    ASSERT_TRUE(final_text.find("two") != std::string::npos);
    fs::remove_all(tmp);
}

TEST(file_lock_does_not_serialize_different_files) {
    /* Полосы не должны превращать инструменты в один большой замок:
     * два РАЗНЫХ файла правятся независимо. Проверяется по времени:
     * с глобальным замком четыре записи по 60 мс заняли бы 240 мс. */
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i) {
        ts.emplace_back([i] {
            file_lock::Guard guard("file-" + std::to_string(i));
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        });
    }
    for (auto& t : ts) t.join();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    ASSERT_TRUE(ms < 200);
}

TEST(file_lock_blocks_another_thread_until_released) {
    /* Смысл семафора — взаимное исключение, и проверяется именно оно:
     * второй поток не входит, пока держится первый. */
    std::atomic<bool> entered{false};
    std::thread t;
    {
        file_lock::Guard held("same-path");
        t = std::thread([&] {
            file_lock::Guard wait("same-path");
            entered = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        /* join() здесь был бы висящим: held держит тот самый мьютекс, за
         * которым ждёт t. Освобождение — отдельным шагом, после него t
         * проходит и завершается. */
        ASSERT_FALSE(entered.load());
    }
    t.join();
    ASSERT_TRUE(entered.load());

    /* Блокировка освободилась: следующий Guard на тот же путь берётся
     * сразу, без зависания. Мьютекс нерекурсивный, и вложенных Guard в
     * коде нет намеренно: вложенность маскировала бы двойное чтение
     * одного файла, а не предотвращала бы гонку. */
    file_lock::Guard again("same-path");
}

/* ======================================================================
 * Сохранение формата
 * ====================================================================== */

TEST(write_file_keeps_crlf_of_existing_file) {
    fs::path t = make_tmp_tree();
    write_file(t / "win.txt", "a\r\nb\r\n");
    init_tools(t);
    std::string r = run_write("win.txt", "x\ny\nz\n");
    ASSERT_TRUE(r.find("[ошибка]") == std::string::npos);
    /* Байт-в-байт: иначе git покажет изменение всего файла. */
    ASSERT_EQ(read_file(t / "win.txt"), std::string("x\r\ny\r\nz\r\n"));
    fs::remove_all(t);
}

TEST(write_file_keeps_bom_of_existing_file) {
    fs::path t = make_tmp_tree();
    write_file(t / "bom.json", std::string("\xEF\xBB\xBF") + "{}\n");
    init_tools(t);
    run_write("bom.json", "{\"a\": 1}\n");
    const std::string got = read_file(t / "bom.json");
    ASSERT_EQ(got.substr(0, 3), std::string("\xEF\xBB\xBF"));
    ASSERT_TRUE(got.find("\"a\"") != std::string::npos);
    fs::remove_all(t);
}

TEST(write_file_writes_new_file_as_given) {
    /* Несуществующего файла не с чем сравнивать: пишем ровно то, что
     * прислали, — иначе запись в /tmp изменила бы формат. */
    fs::path t = make_tmp_tree();
    init_tools(t);
    run_write("fresh.txt", "a\nb\n");
    ASSERT_EQ(read_file(t / "fresh.txt"), std::string("a\nb\n"));
    fs::remove_all(t);
}

TEST(edit_file_keeps_crlf_and_bom) {
    fs::path t = make_tmp_tree();
    write_file(t / "m.php", std::string("\xEF\xBB\xBF") + "a\r\nb\r\nc\r\n");
    init_tools(t);
    json::JsonValue args = json::JsonValue::object();
    args.set("path", "m.php");
    args.set("k", json::JsonValue(2));
    args.set("content", "B");
    std::string r = ToolsRegistry::instance().run("edit_file", args);
    ASSERT_TRUE(r.find("[ошибка]") == std::string::npos);
    ASSERT_EQ(read_file(t / "m.php"),
              std::string("\xEF\xBB\xBF") + "a\r\nB\r\nc\r\n");
    fs::remove_all(t);
}

TEST(apply_patch_keeps_crlf_and_bom) {
    fs::path t = make_tmp_tree();
    write_file(t / "p.txt", std::string("\xEF\xBB\xBF") + "a\r\nb\r\n");
    init_tools(t);
    json::JsonValue args = json::JsonValue::object();
    args.set("patchText",
             "*** Begin Patch\n*** Update File: p.txt\n@@\n-b\n+B\n"
             "*** End Patch\n");
    std::string r = ToolsRegistry::instance().run("apply_patch", args);
    ASSERT_TRUE(r.find("[патч не разобран]") == std::string::npos);
    ASSERT_EQ(read_file(t / "p.txt"), std::string("\xEF\xBB\xBF") + "a\r\nB\r\n");
    fs::remove_all(t);
}

TEST(pending_apply_does_not_deadlock_with_tool_writes) {
    /* Порядок блокировок: инструмент берёт файловый семафор, потом
     * state_.mtx (propose_write). pending_apply зовётся из UI и тоже
     * пишет файл — если бы он держал state_.mtx поверх семафора, два
     * потока ждали бы друг друга вечно. Здесь это проверяется по факту:
     * гонка двух вызовов обязана завершиться. */
    fs::path t = make_tmp_tree();
    write_file(t / "race.txt", "base\n");
    init_tools(t);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().pending.push_back({"race.txt", "from UI\n"});
    }
    std::thread ui([&] { engine().pending_apply(0); });
    std::string tool_result = run_write("race.txt", "from agent\n");
    ui.join();
    ASSERT_TRUE(tool_result.find("[ошибка]") == std::string::npos);
    const std::string got = read_file(t / "race.txt");
    /* Что-то записано, и запись целая (без «половины» файла). */
    ASSERT_TRUE(got == "from UI\n" || got == "from agent\n");
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        ASSERT_TRUE(engine_state().pending.empty());
    }
    fs::remove_all(t);
}
