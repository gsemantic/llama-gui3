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
#include "../core/tool_protocol.h"

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

/* И9.11: legacy-протокол (wp_action) обязан понимать OFFSET/LIMIT.
 *
 * Модель читала файл по описанию read_file, писала `OFFSET: 40`, а парсер
 * знал только PATH/QUERY/ROOT/PATTERN/CONTENT/CLI/URL/K — ключи молча
 * терялись, и read_file возвращал первую страницу. В сессии на калькуляторе
 * это стоило ~10 шагов: модель читала один и тот же файл и не могла
 * добраться до конца, чтобы начать правку. */
TEST(read_tool_legacy_block_parses_offset_and_limit) {
    json::JsonValue args;
    const bool ok = parse_action(
        "wp_action\nTOOL: read_file\nPATH: calculator.py\n"
        "OFFSET: 40\nLIMIT: 90",
        args);
    ASSERT_TRUE(ok);
    ASSERT_EQ(args.get_string("tool"), std::string("read_file"));
    ASSERT_EQ(args.get_int("offset", 0), 40);
    ASSERT_EQ(args.get_int("limit", 0), 90);
}

/* И9.12: DeepSeek DSML — формат вызова, который модель пишет текстом,
 * когда провайдер не отдаёт нативный tool_calls. Плагин его не знал и
 * отвечал «ты не вызвал инструмент», хотя вызов был:
 *
 *   <｜DSML｜ calls>
 *   <｜DSML｜ invoke name="read_file">
 *   <｜DSML｜ parameter name="path" string="true">calculator.py</｜DSML｜ parameter>
 *   </｜DSML｜ invoke>
 *   </｜DSML｜ calls>
 */
TEST(extract_dsml_call_from_deepseek_text) {
    /* Ноль-ширинные пробелы после '<' — DeepSeek вставляет их на
     * практике; без strip_zero_width блок не находится.
     *
     * \x9C и следующий D нельзя писать в одном литерале: \x съедает
     * все hex-цифры подряд, и \x9CD превращается в один неверный байт.
     * Поэтому \xEF\xBD\x9C и "DSML" разделены. */
    const std::string text =
        "<\xE2\x80\x8B\xEF\xBD\x9C" "DSML" "\xEF\xBD\x9C calls>\n"
        "<\xE2\x80\x8B\xEF\xBD\x9C" "DSML" "\xEF\xBD\x9C invoke name=\"read_file\">\n"
        "<\xE2\x80\x8B\xEF\xBD\x9C" "DSML" "\xEF\xBD\x9C parameter name=\"path\" "
        "string=\"true\">calculator.py</\xE2\x80\x8B\xEF\xBD\x9C" "DSML" "\xEF\xBD\x9C "
        "parameter>\n"
        "</\xE2\x80\x8B\xEF\xBD\x9C" "DSML" "\xEF\xBD\x9C invoke>\n"
        "</\xE2\x80\x8B\xEF\xBD\x9C" "DSML" "\xEF\xBD\x9C calls>";
    std::string rest;
    const std::string block = extract_action(text, rest);
    std::cerr << "  DSML block: " << block << std::endl;
    ASSERT_FALSE(block.empty());
    json::JsonValue args;
    ASSERT_TRUE(parse_action(block, args));
    ASSERT_EQ(args.get_string("tool"), std::string("read_file"));
    ASSERT_EQ(args.get_string("path"), std::string("calculator.py"));
    /* Блок не остаётся в тексте ответа. */
    ASSERT_TRUE(rest.find("DSML") == std::string::npos);
}

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

TEST(read_tool_rejects_offset_beyond_eof_with_a_line_hint) {
    /* Сессия 0.18.20, субагент: offset=12121 (размер файла в байтах)
     * возвращал 0 строк, и вызов повторялся. Offset — номер строки. */
    fs::path t = make_tmp_tree();
    write_bytes(t / "n.txt", "1\n2\n3\n");
    init_tools(t);
    const std::string r = read_tool("n.txt", 100, 10);
    ASSERT_TRUE(r.find("за пределами") != std::string::npos);
    ASSERT_TRUE(r.find("3 строк") != std::string::npos);
    ASSERT_TRUE(r.find("НОМЕР СТРОКИ") != std::string::npos || r.find("номер строки") != std::string::npos);
    fs::remove_all(t);
}

/* ======================================================================
 * Б1 / BEHAVIOR_PARITY_PLAN И14.1: номера строк в выводе read_file
 *
 * Зачем: `edit_file` заменяет ДИАПАЗОН СТРОК `k..query`, а модель без
 * номеров считала строки на глаз и промахивалась (ses_000000000131: шесть
 * правок из 13 сломали файл). Проверяется не «есть ли префикс», а три
 * вещи, которые ломаются молча:
 *   1. номер соответствует СТРОКЕ (a не просто «что-то напечатано»);
 *   2. номер 1-based и начинается с `first_line` при offset;
 *   3. номер НЕ попадает в байтовый бюджет дважды (иначе лимит вырос бы
 *      незаметно) — проверяется тем, что файл на границе режется там же,
 *      где и раньше, с поправкой на префикс.
 * ====================================================================== */

TEST(read_tool_prints_line_numbers_for_edit_file) {
    fs::path t = make_tmp_tree();
    write_bytes(t / "n.txt", "alpha\nbeta\ngamma\n");
    init_tools(t);
    const std::string r = read_tool("n.txt");
    /* Номер + табуляция + текст: ровно та форма, из которой edit_file берёт
     * k..query. Проверяем «строка начинается с номера», а не «где-то есть
     * цифра»: иначе префикс мог бы быть в конце и не helping. */
    ASSERT_TRUE(r.find("     1\talpha") != std::string::npos);
    ASSERT_TRUE(r.find("     2\tbeta") != std::string::npos);
    ASSERT_TRUE(r.find("     3\tgamma") != std::string::npos);
    fs::remove_all(t);
}

TEST(read_tool_line_numbers_respect_offset_and_are_one_based) {
    fs::path t = make_tmp_tree();
    write_bytes(t / "n.txt", "1\n2\n3\n4\n5\n");
    init_tools(t);
    /* offset=3, limit=2 → строки 3 и 4, с НОМЕРАМИ 3 и 4 (1-based). */
    const std::string r = read_tool("n.txt", 3, 2);
    ASSERT_TRUE(r.find("     3\t3") != std::string::npos);
    ASSERT_TRUE(r.find("     4\t4") != std::string::npos);
    ASSERT_TRUE(r.find("     2\t2") == std::string::npos);
    fs::remove_all(t);
}

TEST(read_tool_numbers_count_toward_the_byte_budget) {
    /* Бюджет считает и ПРЕФИКС: иначе добавление номеров молча увеличило бы
     * объём, который уходит модели. Материал подобран так, что текст БЕЗ
     * префикса влезал бы целиком, а С префиксом — нет:
     *   400 строк × 46 байт (45 символов + \n) = 18 400 < 20 000  → без
     *     префикса влезло бы;
     *   400 строк × 54 байта (8 префикс + 45 + \n) = 21 600 > 20 000 → с
     *     префиксом обязано обрезаться.
     * Если бюджет НЕ считает префикс, чтение вернёт все 400 строк и тест
     * упадёт — это и есть проверка, а не «должно работать». */
    fs::path t = make_tmp_tree();
    std::string body;
    for (int i = 0; i < 400; ++i) body += std::string(45, 'x') + "\n";
    write_bytes(t / "big.txt", body);
    init_tools(t);
    /* База: без префикса объём меньше бюджета. */
    ASSERT_TRUE(body.size() < limits::kReadFileChars);
    ToolOutput o = ToolsRegistry::instance().run_output("read_file", read_args("big.txt"));
    ASSERT_TRUE(o.metadata.is_object());
    ASSERT_TRUE(o.metadata.get_bool("more", false));
    /* И сказано, сколько строк прочитано: молчаливое «прочитано всё» при
     * обрезке было бы ложью. */
    ASSERT_TRUE(o.metadata.get_int("lines", 0) < 400);
    fs::remove_all(t);
}

TEST(read_tool_numbers_do_not_break_search_replace) {
    /* Номера — только в ВЫВОДЕ read_file. search_replace ищет ТЕКСТ, и
     * префикс не должен попасть в файл: иначе первая же правка по тексту
     * вставила бы «     1\t» в код. */
    fs::path t = make_tmp_tree();
    write_bytes(t / "n.txt", "hello\nworld\n");
    init_tools(t);
    (void)read_tool("n.txt");  /* читаем — ничего не пишет */
    std::ifstream f(t / "n.txt", std::ios::binary);
    const std::string got((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    ASSERT_EQ(got, std::string("hello\nworld\n"));
    fs::remove_all(t);
}
