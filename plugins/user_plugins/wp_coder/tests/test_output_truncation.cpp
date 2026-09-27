/*
 * test_output_truncation.cpp — И4.10: универсальный предел вывода.
 *
 * Проверяется то, ради чего задача и нужна: правило одно и оно не
 * обходится. Инструмент синтетический и специально «вредный»: ни
 * кольцевого буфера, ни собственного лимита у него нет. Если бы правило
 * жило в ToolRunner (как записано в плане), этот инструмент в реальном
 * прогоне остался бы единственным исключением, которое забыли.
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/json.h"
#include "../core/limits.h"
#include "../core/shell.h"
#include "../core/tool.h"
#include "../core/tools_registry.h"
#include "../core/engine.h"
#include "../core/base_tools.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace coder;

namespace {

fs::path make_tmp_tree() {
    fs::path tmp = fs::temp_directory_path()
        / ("wp_coder_trunc_" + std::to_string(::getpid()) + "_"
           + std::to_string(std::rand()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    return tmp;
}

void init_engine(const fs::path& project) {
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&, LlmReply&) { return false; };
    cb.llm_complete = [](const std::string&, const std::string&, std::string&) { return false; };
    cb.llm_is_connected = []() { return false; };
    cb.chat_event = [](const std::string&) {};
    cb.path_data_dir = [&project]() -> std::string { return project.string(); };
    Engine::instance().init(cb);
    test_support::approve_all_permissions();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = project.string();
    }
}

/* Инструмент без собственного лимита: столько, сколько попросили. */
void register_oversized_tool(size_t lines) {
    ToolDef def;
    def.name = "test_oversized";
    def.description = "Синтетический инструмент для проверки усечения";
    def.flags = TF_READ_ONLY;
    def.permission_key = "read";
    def.parameters = SchemaBuilder().build();
    def.handler = [lines](const json::JsonValue&, ToolContext&) -> ToolOutput {
        ToolOutput o;
        o.title = "oversized";
        for (size_t i = 0; i < lines; ++i)
            o.output += "строка " + std::to_string(i) + "\n";
        return o;
    };
    ToolsRegistry::instance().register_def(std::move(def));
}

} // anonymous namespace

TEST(output_limits_are_the_ones_from_the_plan) {
    ASSERT_EQ((size_t)limits::kMaxOutputLines, (size_t)2000);
    ASSERT_EQ((size_t)limits::kMaxOutputBytes, (size_t)50 * 1024);
}

TEST(universal_limit_cuts_lines_and_spills_the_rest) {
    fs::path t = make_tmp_tree();
    init_engine(t);
    register_oversized_tool(5000);

    ToolOutput o = ToolsRegistry::instance().run_output(
        "test_oversized", json::JsonValue::object());

    ASSERT_TRUE(o.truncated);
    ASSERT_TRUE(o.metadata.get_bool("truncated", false));
    /* Модель должна знать и что вывод неполон, и где полный. */
    ASSERT_TRUE(o.output.find("вывод обрезан") != std::string::npos);
    ASSERT_TRUE(o.output.find("строка 0") != std::string::npos);
    ASSERT_TRUE(o.output.find("строка 4999") == std::string::npos);

    const std::string path = o.metadata.get_string("output_path");
    ASSERT_TRUE(!path.empty());
    ASSERT_TRUE(o.output.find(path) != std::string::npos);
    ASSERT_TRUE(fs::exists(path));
    std::ifstream f(path, std::ios::binary);
    const std::string full((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    /* В файле — именно ПОЛНЫЙ вывод: обрезать его и потом его же назвать
     * полным было бы тем же враньём, от которого правило и отнимает силу. */
    ASSERT_TRUE(full.find("строка 4999") != std::string::npos);
    ASSERT_TRUE(full.size() > o.output.size());
    fs::remove_all(t);
}

TEST(universal_limit_leaves_small_output_untouched) {
    fs::path t = make_tmp_tree();
    init_engine(t);
    register_oversized_tool(10);
    ToolOutput o = ToolsRegistry::instance().run_output(
        "test_oversized", json::JsonValue::object());
    ASSERT_FALSE(o.truncated);
    ASSERT_TRUE(o.output.find("строка 9") != std::string::npos);
    ASSERT_TRUE(o.output.find("обрез") == std::string::npos);
    ASSERT_TRUE(o.metadata.get_string("output_path").empty());
    fs::remove_all(t);
}

TEST(universal_limit_cuts_by_bytes_even_when_lines_are_few) {
    fs::path t = make_tmp_tree();
    init_engine(t);
    /* 10 строк по 20 КБ: строк мало, байт много. */
    ToolDef def;
    def.name = "test_wide";
    def.description = "Синтетический инструмент с широкими строками";
    def.flags = TF_READ_ONLY;
    def.permission_key = "read";
    def.parameters = SchemaBuilder().build();
    def.handler = [](const json::JsonValue&, ToolContext&) -> ToolOutput {
        ToolOutput o;
        o.title = "wide";
        for (int i = 0; i < 10; ++i) o.output += std::string(20000, 'x') + "\n";
        return o;
    };
    ToolsRegistry::instance().register_def(std::move(def));

    ToolOutput o = ToolsRegistry::instance().run_output(
        "test_wide", json::JsonValue::object());
    ASSERT_TRUE(o.truncated);
    ASSERT_TRUE(o.output.size() < 10 * 20000);
    ASSERT_TRUE(o.output.find("вывод обрезан") != std::string::npos);
    fs::remove_all(t);
}

TEST(universal_limit_says_so_when_there_is_nowhere_to_spill) {
    fs::path t = make_tmp_tree();
    /* path_data_dir не задан — spill некуда, и молча обрезанный вывод
     * выглядел бы как полный. */
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&, LlmReply&) { return false; };
    cb.llm_complete = [](const std::string&, const std::string&, std::string&) { return false; };
    cb.llm_is_connected = []() { return false; };
    cb.chat_event = [](const std::string&) {};
    Engine::instance().init(cb);
    test_support::approve_all_permissions();
    register_oversized_tool(5000);
    ToolOutput o = ToolsRegistry::instance().run_output(
        "test_oversized", json::JsonValue::object());
    ASSERT_TRUE(o.truncated);
    ASSERT_TRUE(o.output.find("не сохранён") != std::string::npos);
    fs::remove_all(t);
}
