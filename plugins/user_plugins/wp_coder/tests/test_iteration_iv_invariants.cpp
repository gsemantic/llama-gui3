/*
 * test_iteration_iv_invariants.cpp — И4.12: сквозные инварианты итерации.
 *
 * Здесь не проверяется поведение отдельных инструментов (это делают
 * test_glob / test_apply_patch / test_edit_cascade / test_bash_tool /
 * test_output_truncation). Проверяются два утверждения, ради которых
 * итерация и затевалась:
 *
 *   1. НИ ОДИН инструмент не может вернуть неограниченный вывод.
 *      Проверяется на настоящих инструментах с настоящим «мусором» на
 *      входе, потому что предел существует именно для них.
 *
 *   2. Каждый инструмент, пишущий файлы, классифицирован в тесте формата.
 *      Новый пишущий инструмент обязан попасть в таблицу: молча
 *      неклассифицированный инструмент прошёл бы мимо проверки BOM/CRLF, а это
 *      ровно тот класс дефектов, который И4.5 и закрывал.
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/json.h"
#include "../core/limits.h"
#include "../core/tool.h"
#include "../core/tools_registry.h"
#include "../core/engine.h"
#include "../core/base_tools.h"
#include "../core/git_tools.h"
#include "../core/project.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace coder;

namespace {

fs::path make_tmp_tree() {
    fs::path tmp = fs::temp_directory_path()
        / ("wp_coder_inv_" + std::to_string(::getpid()) + "_"
           + std::to_string(std::rand()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    return tmp;
}

void write_bytes(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(body.data(), static_cast<std::streamsize>(body.size()));
}

std::string read_bytes(const fs::path& p) {
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
    cb.path_data_dir = [&project]() -> std::string { return project.string(); };
    Engine::instance().init(cb);
    test_support::approve_all_permissions();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = project.string();
        engine_state().allowed_external_paths.clear();
        engine_state().plan_mode = false;
    }
    register_base_tools();
    register_git_tools();
}

/* Инструменты, которые физически могут вернуть много: у каждого —
 * аргументы, дающие большой результат. */
struct BigOutputCase {
    const char* tool;
    json::JsonValue args;
};

} // anonymous namespace

/* Критерий готовности И4: «ни один инструмент не может вернуть
 * неограниченный вывод». */
TEST(no_tool_returns_unbounded_output) {
    fs::path t = make_tmp_tree();
    init_tools(t);
    /* Файл на 300 КБ — заведомо больше предела (50 КБ). */
    std::string big;
    for (int i = 0; i < 6000; ++i) big += "строка " + std::to_string(i) + "\n";
    write_bytes(t / "big.txt", big);
    write_bytes(t / "big.php", big);

    std::vector<BigOutputCase> cases;
    {
        json::JsonValue a = json::JsonValue::object();
        a.set("path", "big.txt");
        a.set("offset", json::JsonValue(1));
        a.set("limit", json::JsonValue(1000000));
        cases.push_back({"read_file", a});
    }
    {
        json::JsonValue a = json::JsonValue::object();
        a.set("pattern", "**/*");
        a.set("path", ".");
        cases.push_back({"glob", a});
    }
    {
        json::JsonValue a = json::JsonValue::object();
        a.set("path", ".");
        a.set("depth", json::JsonValue(3));
        cases.push_back({"list", a});
    }
    {
        json::JsonValue a = json::JsonValue::object();
        a.set("cli", "seq 1 500000");
        cases.push_back({"bash", a});
    }
    {
        json::JsonValue a = json::JsonValue::object();
        a.set("root", ".");
        a.set("pattern", ".*");
        cases.push_back({"grep_search", a});
    }
    {
        json::JsonValue a = json::JsonValue::object();
        a.set("root", ".");
        cases.push_back({"repo_map", a});
    }

    for (const auto& c : cases) {
        ToolOutput o = ToolsRegistry::instance().run_output(c.tool, c.args);
        /* Запас на служебный блок усечения и метаданные. */
        const size_t ceiling = limits::kMaxOutputBytes + 4096;
        if (o.output.size() > ceiling) {
            std::cerr << "  инструмент " << c.tool << " вернул "
                      << o.output.size() << " байт при пределе "
                      << limits::kMaxOutputBytes << std::endl;
            ASSERT_TRUE(false);
        }
        /* Обрезанный вывод ОБЯЗАН быть объявлен: иначе модель считает
         * его полным. */
        if (o.truncated) {
            /* Помечен обрезанным — обязан сказать об этом словами. Иначе
             * модель читает неполный список как полный. */
            if (o.output.find("обрезан") == std::string::npos &&
                o.output.find("утрачено") == std::string::npos) {
                std::cerr << "  " << c.tool
                          << " помечен обрезанным, но не сказал об этом: ["
                          << o.output.substr(o.output.size() > 200
                                                 ? o.output.size() - 200 : 0)
                          << "]" << std::endl;
            }
            ASSERT_TRUE(o.output.find("обрезан") != std::string::npos ||
                        o.output.find("утрачено") != std::string::npos);
        }
    }
    fs::remove_all(t);
}

/* Каждый пишущий инструмент обязан быть в таблице проверки формата.
 *
 * Список закрыт намеренно: новый инструмент с TF_WRITES_FILES, который
 * никто не классифицировал, прошёл бы мимо проверки BOM/CRLF, а именно
 * её И4.5 и закрывал (правка двух строк превращалась в «переписан весь
 * файл»). */
TEST(every_write_tool_is_classified_by_the_format_test) {
    fs::path t = make_tmp_tree();
    init_tools(t);

    /* Пишущие файлы проекта: проверяются таблицей ниже. */
    static const char* kChecked[] = {"write_file", "edit_file", "search_replace",
                                     "apply_patch"};
    /* Пишущие, но не проверяемые таблицей, с причиной. */
    static const char* kExempt[] = {
        /* И10.3 `revert`: возвращает БАЙТЫ снимка как есть, потому что
         * откат — это не правка. Проверять тут нечего: сравнивать не с
         * чем, эталонного состояния у проверки нет, а нашёлся бы ровно
         * тот дефект (BOM/CRLF), против которого откат и работает. */
        "revert",
        /* Пишет не в проект, а во внешний индекс RAG (через хост). */
        "rag_index"};

    for (const auto& d : ToolsRegistry::instance().defs()) {
        if (!tf_has(d.flags, TF_WRITES_FILES)) continue;
        bool known = false;
        for (const char* n : kChecked) known = known || d.name == n;
        for (const char* n : kExempt) known = known || d.name == n;
        if (!known) {
            std::cerr << "  пишущий инструмент " << d.name
                      << " не классифицирован: добавь его в"
                         " write_tool_preserves_bom_and_crlf или в список"
                         " исключений с причиной" << std::endl;
            ASSERT_TRUE(false);
        }
    }

    /* write_file: полная перезапись. */
    write_bytes(t / "w.txt", "\xEF\xBB\xBF" "a\r\nb\r\n");
    json::JsonValue w = json::JsonValue::object();
    w.set("path", "w.txt");
    w.set("content", "x\ny\nz\n");
    ToolsRegistry::instance().run_output("write_file", w);
    ASSERT_EQ(read_bytes(t / "w.txt"),
              std::string("\xEF\xBB\xBF") + "x\r\ny\r\nz\r\n");

    /* edit_file: диапазон строк. */
    write_bytes(t / "e.txt", "\xEF\xBB\xBF" "a\r\nb\r\nc\r\n");
    json::JsonValue e = json::JsonValue::object();
    e.set("path", "e.txt");
    e.set("k", json::JsonValue(2));
    e.set("content", "B");
    ToolsRegistry::instance().run_output("edit_file", e);
    ASSERT_EQ(read_bytes(t / "e.txt"),
              std::string("\xEF\xBB\xBF") + "a\r\nB\r\nc\r\n");

    /* search_replace: замена внутри строки. */
    write_bytes(t / "s.txt", "\xEF\xBB\xBF" "a\r\nold\r\n");
    json::JsonValue s = json::JsonValue::object();
    s.set("path", "s.txt");
    s.set("query", "old");
    s.set("content", "new");
    ToolsRegistry::instance().run_output("search_replace", s);
    ASSERT_EQ(read_bytes(t / "s.txt"),
              std::string("\xEF\xBB\xBF") + "a\r\nnew\r\n");

    /* apply_patch: хук. */
    write_bytes(t / "p.txt", "\xEF\xBB\xBF" "a\r\nold\r\n");
    json::JsonValue p = json::JsonValue::object();
    p.set("patchText", "*** Begin Patch\n*** Update File: p.txt\n@@\n-old\n+new\n"
                       "*** End Patch\n");
    ToolsRegistry::instance().run_output("apply_patch", p);
    ASSERT_EQ(read_bytes(t / "p.txt"),
              std::string("\xEF\xBB\xBF") + "a\r\nnew\r\n");
    fs::remove_all(t);
}

/* Критерий набора инструментов: имена, которые ждёт модель, есть в
 * реестре и описаны в каталоге (иначе инструмент есть, а модель о нём
 * не знает — это был дефект D2 для exec_command). */
TEST(expected_tool_set_is_registered_and_documented) {
    fs::path t = make_tmp_tree();
    init_tools(t);
    register_rag_tools();
    const std::string cat = ToolsRegistry::instance().build_tool_catalogue();
    for (const char* name : {"read_file", "write_file", "search_replace",
                             "apply_patch", "glob", "list", "grep_search",
                             "bash", "todowrite", "todoread",
                             /* И10.3: откат по снимку. В списке ожидаемого
                              * набора, а не «есть в реестре»: инструмент,
                              * которого модель не видит в каталоге, не
                              * отличается от отсутствующего. */
                             "revert"}) {
        if (!ToolsRegistry::instance().has(name)) {
            std::cerr << "  нет инструмента " << name << std::endl;
            ASSERT_TRUE(false);
        }
        if (cat.find(std::string("- ") + name + " —") == std::string::npos) {
            std::cerr << "  инструмент " << name
                      << " есть в реестре, но его нет в каталоге промпта"
                      << std::endl;
            ASSERT_TRUE(false);
        }
    }
    fs::remove_all(t);
}

/* Пределы итерации лежат в core/limits.h, и числа не разъезжаются по
 * файлам: это болезнь D12 (версия в четырёх местах). */
TEST(iteration_limits_live_in_one_place) {
    ASSERT_EQ((int)limits::kMaxOutputLines, 2000);
    ASSERT_EQ((int)limits::kMaxOutputBytes, 50 * 1024);
    ASSERT_EQ((int)limits::kShellTimeoutSec, 120);
    ASSERT_EQ((int)limits::kReadDefaultLines, 2000);
    ASSERT_EQ((int)limits::kGlobLimit, 100);
    ASSERT_EQ((int)limits::kListLimit, 300);
    ASSERT_EQ((int)limits::kMaxToolOutput, 12000);
    ASSERT_EQ((int)limits::kReadFileChars, 12000);
}
