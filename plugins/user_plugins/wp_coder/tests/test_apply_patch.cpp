/*
 * test_apply_patch.cpp — И4.2: патч в формате opencode.
 *
 * Проверяются три разных слоя, потому что ломаются они по-разному:
 *   - разбор: если он пропускает мусор, Applying дальше нечего — и модель
 *     получает отказ там, где патч был правильным;
 *   - применение хуков: главная беда — «нашлось не там», и проверяется
 *     это только на файле с ДВУМЯ одинаковыми кусками;
 *   - инструмент: запись, режим плана, отказ по небезопасному пути и
 *     отказ целиком, а не наполовину.
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/apply_patch.h"
#include "../core/text_edit.h"
#include "../core/json.h"
#include "../core/tool.h"
#include "../core/tools_registry.h"
#include "../core/engine.h"
#include "../core/base_tools.h"
#include "../core/project.h"

#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace coder;

namespace {

fs::path make_tmp_tree() {
    fs::path tmp = fs::temp_directory_path()
        / ("wp_coder_patch_" + std::to_string(::getpid()) + "_"
           + std::to_string(std::rand()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
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
    }
    register_base_tools();
}

struct PlanModeGuard {
    bool prev;
    explicit PlanModeGuard(bool on) {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        prev = engine_state().plan_mode;
        engine_state().plan_mode = on;
    }
    ~PlanModeGuard() {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().plan_mode = prev;
    }
};

std::string run_patch(const std::string& text) {
    json::JsonValue args = json::JsonValue::object();
    args.set("patchText", text);
    return ToolsRegistry::instance().run("apply_patch", args);
}

} // anonymous namespace

/* ======================================================================
 * Разбор
 * ====================================================================== */

TEST(patch_parses_all_four_operations) {
    const std::string text =
        "*** Begin Patch\n"
        "*** Add File: src/new.php\n"
        "+<?php\n"
        "+echo 1;\n"
        "*** Update File: src/old.php\n"
        "*** Move to: src/renamed.php\n"
        "@@ function wp_footer\n"
        " echo 'a';\n"
        "-echo 'b';\n"
        "+echo 'c';\n"
        "*** Delete File: src/gone.php\n"
        "*** End Patch\n";
    patch::Parsed p = patch::parse(text);
    ASSERT_TRUE(p.ok());
    ASSERT_EQ(p.files.size(), (size_t)3);

    ASSERT_TRUE(p.files[0].op == patch::Op::Add);
    ASSERT_EQ(p.files[0].path, std::string("src/new.php"));
    ASSERT_EQ(p.files[0].hunks.size(), (size_t)1);
    ASSERT_EQ(p.files[0].hunks[0].new_lines.size(), (size_t)2);
    ASSERT_EQ(p.files[0].hunks[0].new_lines[0], std::string("<?php"));

    ASSERT_TRUE(p.files[1].op == patch::Op::Update);
    ASSERT_EQ(p.files[1].move_to, std::string("src/renamed.php"));
    ASSERT_EQ(p.files[1].hunks.size(), (size_t)1);
    /* Внутренняя модель Hunk: old = контекст+удаления, new = контекст+добавления */
    const patch::Hunk& h = p.files[1].hunks[0];
    ASSERT_EQ(h.change_context, std::string("function wp_footer"));
    ASSERT_EQ(h.old_lines.size(), (size_t)2);
    ASSERT_EQ(h.old_lines[0], std::string("echo 'a';"));
    ASSERT_EQ(h.old_lines[1], std::string("echo 'b';"));
    ASSERT_EQ(h.new_lines.size(), (size_t)2);
    ASSERT_EQ(h.new_lines[1], std::string("echo 'c';"));
    ASSERT_FALSE(h.is_end_of_file);

    ASSERT_TRUE(p.files[2].op == patch::Op::Delete);
    ASSERT_EQ(p.files[2].path, std::string("src/gone.php"));
    ASSERT_TRUE(p.files[2].hunks.empty());
}

TEST(patch_parses_several_hunks_in_one_file) {
    patch::Parsed p = patch::parse(
        "*** Begin Patch\n"
        "*** Update File: a.txt\n"
        "@@ first\n"
        "-one\n"
        "+ONE\n"
        "@@ second\n"
        " keep\n"
        "-two\n"
        "+TWO\n"
        "*** End Patch\n");
    ASSERT_TRUE(p.ok());
    ASSERT_EQ(p.files[0].hunks.size(), (size_t)2);
    ASSERT_EQ(p.files[0].hunks[0].change_context, std::string("first"));
    ASSERT_EQ(p.files[0].hunks[1].change_context, std::string("second"));
    ASSERT_FALSE(p.files[0].hunks[1].is_end_of_file);
}

TEST(patch_parses_end_of_file_marker) {
    patch::Parsed p = patch::parse(
        "*** Begin Patch\n"
        "*** Update File: a.txt\n"
        "@@\n"
        "+дописано\n"
        "*** End of File\n"
        "*** End Patch\n");
    ASSERT_TRUE(p.ok());
    const patch::Hunk& h = p.files[0].hunks[0];
    ASSERT_TRUE(h.is_end_of_file);
    /* Пустой old_lines + End of File = «дописать в конец»: иначе
     * «добавить строку в конец файла» невыразимо — искать нечего. */
    ASSERT_TRUE(h.old_lines.empty());
    ASSERT_EQ(h.new_lines.size(), (size_t)1);
}

TEST(patch_rejects_broken_input_with_actionable_error) {
    struct Case { const char* text; const char* must_contain; };
    const Case cases[] = {
        {"*** Update File: a.txt\n@@\n-a\n+b\n",
         "*** Begin Patch"},
        {"*** Begin Patch\n*** Update File: a.txt\n@@\n-a\n+b\n",
         "*** End Patch"},
        {"*** Begin Patch\n*** Update File: a.txt\n@@\n-a\n+b\n"
         "*** End Patch\nещё текст\n",
         "остался текст"},
        {"*** Begin Patch\n*** Frobnicate File: a.txt\n*** End Patch\n",
         "неизвестная директива"},
        {"*** Begin Patch\n*** Update File: a.txt\n@@\nмусор\n"
         "*** End Patch\n",
         "не начинается"},
        {"*** Begin Patch\n*** Add File: a.txt\nбез плюса\n*** End Patch\n",
         "должна начинаться с '+'"},
        {"*** Begin Patch\n*** Delete File: a.txt\n+мусор\n*** End Patch\n",
         "не принимает содержимое"},
        {"*** Begin Patch\n*** Move to: b.txt\n*** End Patch\n",
         "только сразу после"},
        {"*** Begin Patch\n*** End Patch\n", "нет ни одной операции"},
    };
    for (const auto& c : cases) {
        patch::Parsed p = patch::parse(c.text);
        if (p.ok() || p.error.find(c.must_contain) == std::string::npos) {
            std::cerr << "  ожидалась ошибка с '" << c.must_contain
                      << "', получено: '" << p.error << "'" << std::endl;
            ASSERT_TRUE(false);
        }
        /* Ошибка разбора означает, что применять нечего: половина патча
         * хуже, чем отказ. */
        ASSERT_TRUE(p.files.empty());
    }
}

/* ======================================================================
 * Применение хуков
 * ====================================================================== */

TEST(apply_hunks_replaces_block_and_keeps_the_rest) {
    TextFile f = split_text("one\ntwo\nthree\n");
    patch::Applied a = patch::apply_hunks(
        f, {patch::Hunk{{"two"}, {"TWO"}, "", false}});
    ASSERT_TRUE(a.ok);
    ASSERT_EQ(join_text(a.file), std::string("one\nTWO\nthree\n"));
}

TEST(apply_hunks_applies_several_hunks_in_order) {
    TextFile f = split_text("a\nb\nc\nd\n");
    std::vector<patch::Hunk> hunks;
    hunks.push_back(patch::Hunk{{"a"}, {"A"}, "", false});
    hunks.push_back(patch::Hunk{{"d"}, {"D", "E"}, "", false});
    patch::Applied a = patch::apply_hunks(f, hunks);
    ASSERT_TRUE(a.ok);
    ASSERT_EQ(join_text(a.file), std::string("A\nb\nc\nD\nE\n"));
}

TEST(apply_hunks_uses_context_hint_when_block_is_not_unique) {
    /* Два одинаковых куска в файле: без подсказки контекста правка попала
     * бы в первый — то есть «молча не туда». */
    TextFile f = split_text("function a() {\n  return 1;\n}\n"
                            "function b() {\n  return 1;\n}\n");
    patch::Hunk h{{"  return 1;", "}"}, {"  return 2;", "}"}, "function b", false};
    patch::Applied a = patch::apply_hunks(f, {h});
    ASSERT_TRUE(a.ok);
    ASSERT_EQ(join_text(a.file),
              std::string("function a() {\n  return 1;\n}\n"
                          "function b() {\n  return 2;\n}\n"));
}

TEST(apply_hunks_tolerates_trailing_whitespace_but_not_wrong_text) {
    TextFile f = split_text("foo();\nbar();\n");
    /* Хвостовые пробелы в присланном контексте — не повод отказывать. */
    patch::Applied ok_a = patch::apply_hunks(
        f, {patch::Hunk{{"foo();   "}, {"foo(1);"}, "", false}});
    ASSERT_TRUE(ok_a.ok);
    ASSERT_EQ(join_text(ok_a.file), std::string("foo(1);\nbar();\n"));

    /* А вот другой текст — повод. Молча «найти похожее» здесь нельзя. */
    patch::Applied bad = patch::apply_hunks(
        f, {patch::Hunk{{"qux();"}, {"qux(1);"}, "", false}});
    ASSERT_FALSE(bad.ok);
    ASSERT_TRUE(bad.error.find("не найден блок") != std::string::npos);
    /* Ошибка actionable: сказано, что читать заново. */
    ASSERT_TRUE(bad.error.find("Прочитай его заново") != std::string::npos);
    ASSERT_EQ(bad.failed_hunk, (size_t)0);
}

TEST(apply_hunks_appends_at_end_of_file) {
    TextFile f = split_text("a\nb\n");
    patch::Applied app = patch::apply_hunks(
        f, {patch::Hunk{{}, {"c"}, "", true}});
    ASSERT_TRUE(app.ok);
    ASSERT_EQ(join_text(app.file), std::string("a\nb\nc\n"));

    /* Без маркера End of File пустой хук бессмыслен — и это ошибка, а не
     * тихая пустая правка. */
    patch::Applied bad = patch::apply_hunks(f, {patch::Hunk{{}, {"c"}, "", false}});
    ASSERT_FALSE(bad.ok);
    ASSERT_TRUE(bad.error.find("*** End of File") != std::string::npos);
}

TEST(apply_hunks_end_of_file_requires_block_at_the_very_end) {
    TextFile f = split_text("a\nb\nc\n");
    patch::Hunk h{{"a"}, {"A"}, "", true};
    patch::Applied a = patch::apply_hunks(f, {h});
    ASSERT_FALSE(a.ok);
    ASSERT_TRUE(a.error.find("ожидался конец файла") != std::string::npos);
}

TEST(apply_hunks_preserves_bom_and_crlf) {
    /* Файл в Windows-стиле: правка не должна превратить его в Unix-файл,
     * иначе git diff покажет изменение всего файла. */
    const std::string raw = "\xEF\xBB\xBF" "a;\r\nb;\r\n";
    TextFile f = split_text(raw);
    ASSERT_EQ(f.bom, std::string("\xEF\xBB\xBF"));
    ASSERT_TRUE(f.crlf);
    patch::Applied a = patch::apply_hunks(
        f, {patch::Hunk{{"b;"}, {"B;"}, "", false}});
    ASSERT_TRUE(a.ok);
    ASSERT_EQ(join_text(a.file), std::string("\xEF\xBB\xBF" "a;\r\nB;\r\n"));
}

TEST(apply_hunks_preserves_missing_final_newline) {
    TextFile f = split_text("a\nb");
    ASSERT_FALSE(f.trailing_newline);
    patch::Applied a = patch::apply_hunks(
        f, {patch::Hunk{{"b"}, {"b", "c"}, "", false}});
    ASSERT_TRUE(a.ok);
    ASSERT_EQ(join_text(a.file), std::string("a\nb\nc"));
}

TEST(apply_hunks_inserts_into_empty_file) {
    TextFile f = split_text("");
    ASSERT_TRUE(f.lines.empty());
    patch::Applied a = patch::apply_hunks(
        f, {patch::Hunk{{}, {"hello"}, "", true}});
    ASSERT_TRUE(a.ok);
    ASSERT_EQ(join_text(a.file), std::string("hello\n"));
}

/* ======================================================================
 * Инструмент
 * ====================================================================== */

TEST(apply_patch_tool_is_registered_and_documented) {
    fs::path t = make_tmp_tree();
    init_tools(t);
    const ToolDef* d = ToolsRegistry::instance().find("apply_patch");
    ASSERT_TRUE(d != nullptr);
    ASSERT_TRUE(tf_has(d->flags, TF_WRITES_FILES));
    ASSERT_EQ(permission_key_of(*d), std::string("write"));
    std::string cat = ToolsRegistry::instance().build_tool_catalogue();
    size_t at = cat.find("- apply_patch —");
    ASSERT_TRUE(at != std::string::npos);
    size_t end = cat.find("\n- ", at + 1);
    std::string block = cat.substr(at + 1, (end == std::string::npos
                                           ? cat.size() : end) - at - 1);
    ASSERT_TRUE(block.find("patchText") != std::string::npos);
    ASSERT_TRUE(block.find("Begin Patch") != std::string::npos);
    fs::remove_all(t);
}

TEST(apply_patch_writes_files_on_disk) {
    fs::path t = make_tmp_tree();
    write_file(t / "a.txt", "one\ntwo\n");
    init_tools(t);
    std::string r = run_patch(
        "*** Begin Patch\n"
        "*** Add File: sub/b.txt\n"
        "+создан\n"
        "*** Update File: a.txt\n"
        "@@\n"
        " one\n"
        "-two\n"
        "+TWO\n"
        "*** End Patch\n");
    ASSERT_TRUE(r.find("[патч не разобран]") == std::string::npos);
    ASSERT_EQ(read_file(t / "sub" / "b.txt"), std::string("создан\n"));
    ASSERT_EQ(read_file(t / "a.txt"), std::string("one\nTWO\n"));
    /* Правка оставила .orig — этим пользуется undo_edit. */
    ASSERT_TRUE(fs::exists(t / "a.txt.orig"));
    ASSERT_TRUE(r.find("создано 1") != std::string::npos);
    ASSERT_TRUE(r.find("изменено 1") != std::string::npos);
    fs::remove_all(t);
}

TEST(apply_patch_deletes_and_moves) {
    fs::path t = make_tmp_tree();
    write_file(t / "gone.txt", "x\n");
    write_file(t / "old.txt", "hello\n");
    init_tools(t);
    std::string r = run_patch(
        "*** Begin Patch\n"
        "*** Delete File: gone.txt\n"
        "*** Update File: old.txt\n"
        "*** Move to: new.txt\n"
        "@@\n"
        "-hello\n"
        "+привет\n"
        "*** End Patch\n");
    ASSERT_TRUE(r.find("[патч не разобран]") == std::string::npos);
    ASSERT_FALSE(fs::exists(t / "gone.txt"));
    ASSERT_FALSE(fs::exists(t / "old.txt"));
    ASSERT_EQ(read_file(t / "new.txt"), std::string("привет\n"));
    ASSERT_TRUE(r.find("удалено 1") != std::string::npos);
    ASSERT_TRUE(r.find("перемещено 1") != std::string::npos);
    fs::remove_all(t);
}

TEST(apply_patch_refuses_to_overwrite_existing_file_with_add) {
    fs::path t = make_tmp_tree();
    write_file(t / "a.txt", "старое\n");
    init_tools(t);
    std::string r = run_patch(
        "*** Begin Patch\n*** Add File: a.txt\n+новое\n*** End Patch\n");
    ASSERT_TRUE(r.find("уже существует") != std::string::npos);
    /* Файл не тронут: отказ «наполовину» здесь недопустим. */
    ASSERT_EQ(read_file(t / "a.txt"), std::string("старое\n"));
    fs::remove_all(t);
}

TEST(apply_patch_refuses_whole_patch_when_context_does_not_match) {
    fs::path t = make_tmp_tree();
    write_file(t / "a.txt", "one\ntwo\n");
    write_file(t / "b.txt", "b\n");
    init_tools(t);
    std::string r = run_patch(
        "*** Begin Patch\n"
        "*** Update File: a.txt\n"
        "@@\n"
        "-two\n"
        "+TWO\n"
        "*** Update File: b.txt\n"
        "@@\n"
        "-НЕТ ТАКОЙ СТРОКИ\n"
        "+x\n"
        "*** End Patch\n");
    ASSERT_TRUE(r.find("ОТКАЗ — патч применён НЕ полностью") != std::string::npos);
    ASSERT_TRUE(r.find("b.txt") != std::string::npos);
    /* a.txt уже изменён — и об этом сказано прямо, а не спрятано:
     * иначе модель и пользователь решили бы, что не тронуто ничего. */
    ASSERT_EQ(read_file(t / "a.txt"), std::string("one\nTWO\n"));
    ASSERT_TRUE(r.find("Уже записано") != std::string::npos);
    ASSERT_TRUE(r.find("a.txt") != std::string::npos);
    fs::remove_all(t);
}

TEST(apply_patch_refuses_unsafe_path_before_touching_anything) {
    fs::path t = make_tmp_tree();
    write_file(t / "a.txt", "one\n");
    init_tools(t);
    std::string r = run_patch(
        "*** Begin Patch\n"
        "*** Update File: a.txt\n"
        "@@\n"
        "-one\n"
        "+ONE\n"
        "*** Add File: ../escaped.txt\n"
        "+побег\n"
        "*** End Patch\n");
    ASSERT_TRUE(r.find("ничего не изменено") != std::string::npos);
    ASSERT_EQ(read_file(t / "a.txt"), std::string("one\n"));
    ASSERT_FALSE(fs::exists(t.parent_path() / "escaped.txt"));
    fs::remove_all(t);
}

TEST(apply_patch_missing_file_is_reported_with_a_way_out) {
    fs::path t = make_tmp_tree();
    init_tools(t);
    std::string r = run_patch(
        "*** Begin Patch\n"
        "*** Update File: нет-такого.txt\n"
        "@@\n"
        "-a\n"
        "+b\n"
        "*** End Patch\n");
    ASSERT_TRUE(r.find("файла нет") != std::string::npos);
    /* Подсказка, что делать: иначе модель повторит тот же вызов. */
    ASSERT_TRUE(r.find("glob") != std::string::npos);
    fs::remove_all(t);
}

TEST(apply_patch_in_plan_mode_proposes_instead_of_writing) {
    fs::path t = make_tmp_tree();
    write_file(t / "a.txt", "one\n");
    init_tools(t);
    PlanModeGuard plan(true);
    std::string r = run_patch(
        "*** Begin Patch\n"
        "*** Add File: b.txt\n"
        "+новое\n"
        "*** Update File: a.txt\n"
        "@@\n"
        "-one\n"
        "+ONE\n"
        "*** End Patch\n");
    ASSERT_TRUE(r.find("предложено 2") != std::string::npos);
    /* Ничего не записано: режим плана для этого и существует. */
    ASSERT_EQ(read_file(t / "a.txt"), std::string("one\n"));
    ASSERT_FALSE(fs::exists(t / "b.txt"));
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        ASSERT_EQ(engine_state().pending.size(), (size_t)2);
        engine_state().pending.clear();
    }
    fs::remove_all(t);
}

TEST(apply_patch_in_plan_mode_refuses_delete_and_move) {
    fs::path t = make_tmp_tree();
    write_file(t / "a.txt", "x\n");
    init_tools(t);
    PlanModeGuard plan(true);
    std::string r = run_patch(
        "*** Begin Patch\n*** Delete File: a.txt\n*** End Patch\n");
    ASSERT_TRUE(r.find("не применён") != std::string::npos);
    ASSERT_TRUE(fs::exists(t / "a.txt"));
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().pending.clear();
    }
    fs::remove_all(t);
}
