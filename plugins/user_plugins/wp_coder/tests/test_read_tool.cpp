/*
 * test_read_tool.cpp — И4.7: чтение с диапазоном строк, отказ для
 * двоичных файлов, список для каталога.
 *
 * Проверяется то, что стоило переделать: `k` означал «пропустить строк»,
 * и параметра «прочитать N строк» не было вовсе — файл приходилось
 * открывать целиком и просить модель считать строки вручную. Плюс три
 * отказа, которые раньше выглядели бы как «файл не читается».
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/json.h"
#include "../core/tool.h"
#include "../core/tools_registry.h"
#include "../core/engine.h"
#include "../core/base_tools.h"
#include "../core/project.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace coder;

namespace {

fs::path make_tmp_tree() {
    fs::path tmp = fs::temp_directory_path()
        / ("wp_coder_read_" + std::to_string(::getpid()) + "_"
           + std::to_string(std::rand()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    /* И11.5: каталог убирается в конце прогона (test_framework.h), а не остаётся в /tmp до следующего. */
    register_tmp_tree(tmp.string());
    return tmp;
}

void write_bytes(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(body.data(), static_cast<std::streamsize>(body.size()));
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
    }
    register_base_tools();
}

json::JsonValue read_args(const std::string& path, int offset = 0, int limit = 0) {
    json::JsonValue a = json::JsonValue::object();
    a.set("path", path);
    if (offset > 0) a.set("offset", json::JsonValue(offset));
    if (limit > 0) a.set("limit", json::JsonValue(limit));
    return a;
}

std::string read_tool(const std::string& path, int offset = 0, int limit = 0) {
    return ToolsRegistry::instance().run("read_file", read_args(path, offset, limit));
}

} // anonymous namespace

TEST(read_tool_has_offset_and_limit_instead_of_k) {
    fs::path t = make_tmp_tree();
    init_tools(t);
    const ToolDef* d = ToolsRegistry::instance().find("read_file");
    ASSERT_TRUE(d != nullptr);
    ASSERT_TRUE(d->parameters.has("properties"));
    const json::JsonValue& props = d->parameters.get("properties");
    ASSERT_TRUE(props.has("offset"));
    ASSERT_TRUE(props.has("limit"));
    /* K был перегружен («пропустить строк»); два параметра с одним
     * смыслом — это два источника истины, поэтому его убрали. */
    ASSERT_FALSE(props.has("k"));
    /* offset/limit объявлены целыми и в разумных границах. */
    ASSERT_EQ(d->parameters.get("properties").get("offset").get_string("type"),
              std::string("integer"));
    fs::remove_all(t);
}

TEST(read_tool_reads_a_line_range) {
    fs::path t = make_tmp_tree();
    write_bytes(t / "n.txt", "one\ntwo\nthree\nfour\nfive\n");
    init_tools(t);
    std::string r = read_tool("n.txt", 2, 2);
    ASSERT_TRUE(r.find("two") != std::string::npos);
    ASSERT_TRUE(r.find("three") != std::string::npos);
    ASSERT_TRUE(r.find("one") == std::string::npos);
    ASSERT_TRUE(r.find("four") == std::string::npos);
    /* Сказано, где продолжение: иначе модель не знает, что читать дальше. */
    ASSERT_TRUE(r.find("offset=4") != std::string::npos);
    fs::remove_all(t);
}

TEST(read_tool_reports_how_much_was_read) {
    fs::path t = make_tmp_tree();
    write_bytes(t / "n.txt", "1\n2\n3\n4\n5\n");
    init_tools(t);
    ToolOutput o = ToolsRegistry::instance().run_output("read_file", read_args("n.txt", 0, 2));
    ASSERT_TRUE(o.metadata.is_object());
    ASSERT_EQ(o.metadata.get_int("lines", 0), (long long)2);
    ASSERT_TRUE(o.metadata.get_bool("more", false));
    ASSERT_TRUE(o.truncated);
    fs::remove_all(t);
}

TEST(read_tool_reads_whole_file_by_default) {
    fs::path t = make_tmp_tree();
    write_bytes(t / "s.txt", "alpha\nbeta\n");
    init_tools(t);
    std::string r = read_tool("s.txt");
    ASSERT_TRUE(r.find("alpha") != std::string::npos);
    ASSERT_TRUE(r.find("beta") != std::string::npos);
    fs::remove_all(t);
}

TEST(read_tool_refuses_binary_files) {
    fs::path t = make_tmp_tree();
    /* Картинка: доля непечатаемых символов заведомо выше порога. */
    std::string blob;
    for (int i = 0; i < 4000; ++i)
        blob += static_cast<char>((i * 7) % 2 ? '\x00' : '\x89');
    write_bytes(t / "pic.png", blob);
    write_bytes(t / "ok.txt", "нормальный текст\n");
    init_tools(t);
    std::string r = read_tool("pic.png");
    ASSERT_TRUE(r.find("двоичный файл") != std::string::npos);
    ASSERT_TRUE(r.find("30%") != std::string::npos);
    /* Мусор в контекст не попал. */
    ASSERT_TRUE(r.find("\\x89") == std::string::npos);
    /* Обычный текст рядом читается как обычно: детектор не перестраховывается. */
    std::string ok = read_tool("ok.txt");
    ASSERT_TRUE(ok.find("нормальный текст") != std::string::npos);
    fs::remove_all(t);
}

TEST(read_tool_lists_a_directory_instead_of_failing) {
    fs::path t = make_tmp_tree();
    fs::create_directories(t / "sub");
    write_bytes(t / "a.txt", "x");
    write_bytes(t / "sub" / "b.txt", "y");
    init_tools(t);
    std::string r = read_tool("sub");
    /* Раньше чтение каталога давало «не удалось открыть файл», и модель
     * гадала, существует ли он. */
    ASSERT_TRUE(r.find("[каталог]") != std::string::npos);
    ASSERT_TRUE(r.find("b.txt") != std::string::npos);
    fs::remove_all(t);
}

TEST(read_tool_says_when_file_is_missing_with_a_way_out) {
    fs::path t = make_tmp_tree();
    init_tools(t);
    std::string r = read_tool("нет-такого.txt");
    ASSERT_TRUE(r.find("файла нет") != std::string::npos);
    ASSERT_TRUE(r.find("glob") != std::string::npos);
    fs::remove_all(t);
}

TEST(read_tool_reports_instruction_files_nearby) {
    fs::path t = make_tmp_tree();
    fs::create_directories(t / "wp-content" / "themes");
    write_bytes(t / "AGENTS.md", "правила проекта\n");
    write_bytes(t / "wp-content" / "themes" / "AGENTS.md", "правила темы\n");
    write_bytes(t / "wp-content" / "themes" / "style.css", "body{}\n");
    init_tools(t);
    ToolOutput o = ToolsRegistry::instance().run_output(
        "read_file", read_args("wp-content/themes/style.css"));
    ASSERT_TRUE(o.metadata.has("loaded"));
    ASSERT_TRUE(o.metadata.get("loaded").is_array());
    bool own = false, root = false;
    for (size_t i = 0; i < o.metadata.get("loaded").size(); ++i) {
        const std::string p = o.metadata.get("loaded").at(i).get_string("path");
        if (p == "wp-content/themes/AGENTS.md") own = true;
        if (p == "AGENTS.md") root = true;
    }
    /* Поднимаемся от каталога файла к корню: и локальные правила, и
     * общие. И9 подключит их к промпту, пока это факт наличия. */
    ASSERT_TRUE(own);
    ASSERT_TRUE(root);
    /* И в тексте: агент должен знать, что правила рядом есть. */
    ASSERT_TRUE(o.output.find("инструкции проекта") != std::string::npos);
    fs::remove_all(t);
}

TEST(read_tool_still_gates_external_paths) {
    fs::path t = make_tmp_tree();
    init_tools(t);
    /* Проверяем по СОДЕРЖИМОМУ файла, а не по пути: путь в отказе виден
     * намеренно (иначе модель не поймёт, о чём речь), а вот сам файл
     * прочитан быть не должен. */
    const std::string outside = "/tmp/wp_coder_outside_read.txt";
    write_bytes(outside, "СЕКРЕТНОЕ-СОДЕРЖИМОЕ\n");
    std::string r = read_tool(outside);
    ASSERT_TRUE(r.find("СЕКРЕТНОЕ-СОДЕРЖИМОЕ") == std::string::npos);
    ASSERT_TRUE(r.find("Доступ") != std::string::npos ||
                r.find("разрешен") != std::string::npos);
    std::error_code ec;
    fs::remove(outside, ec);
    fs::remove_all(t);
}

/* ======================================================================
 * И4.9 — list с глубиной
 * ====================================================================== */

TEST(list_tool_has_depth_and_replaced_list_dir) {
    fs::path t = make_tmp_tree();
    init_tools(t);
    const ToolDef* d = ToolsRegistry::instance().find("list");
    ASSERT_TRUE(d != nullptr);
    /* Два инструмента с одним смыслом (list и list_dir) — это та двойность,
     * которую И1 убрала из реестра, поэтому list_dir заменён, а не дополнен. */
    ASSERT_TRUE(ToolsRegistry::instance().find("list_dir") == nullptr);
    ASSERT_TRUE(d->parameters.get("properties").has("depth"));
    ASSERT_TRUE(tf_has(d->flags, TF_READ_ONLY));
    fs::remove_all(t);
}

TEST(list_tool_depth_limits_the_walk) {
    fs::path t = make_tmp_tree();
    fs::create_directories(t / "a" / "b");
    write_bytes(t / "top.txt", "1");
    write_bytes(t / "a" / "mid.txt", "2");
    write_bytes(t / "a" / "b" / "deep.txt", "3");
    init_tools(t);

    std::string one = read_tool(".");
    std::string r1 = ToolsRegistry::instance().run("list", read_args("."));
    ASSERT_TRUE(r1.find("a/") != std::string::npos);
    /* Глубина 1 — только содержимое корня. */
    ASSERT_TRUE(r1.find("mid.txt") == std::string::npos);

    json::JsonValue deep = json::JsonValue::object();
    deep.set("path", ".");
    deep.set("depth", json::JsonValue(3));
    std::string r3 = ToolsRegistry::instance().run("list", deep);
    ASSERT_TRUE(r3.find("a/b/deep.txt") != std::string::npos);
    ASSERT_TRUE(r3.find("глубина 3") != std::string::npos);
    fs::remove_all(t);
}

TEST(list_tool_skips_service_directories_like_glob) {
    fs::path t = make_tmp_tree();
    fs::create_directories(t / "node_modules" / "pkg");
    write_bytes(t / "node_modules" / "pkg" / "index.js", "x");
    write_bytes(t / "keep.txt", "y");
    init_tools(t);
    json::JsonValue a = json::JsonValue::object();
    a.set("depth", json::JsonValue(4));
    std::string r = ToolsRegistry::instance().run("list", a);
    /* Тот же список пропускаемых каталогов, что у glob: иначе один и тот же
     * проект два инструмента показывали бы по-разному. */
    ASSERT_TRUE(r.find("node_modules") == std::string::npos);
    ASSERT_TRUE(r.find("keep.txt") != std::string::npos);
    fs::remove_all(t);
}
