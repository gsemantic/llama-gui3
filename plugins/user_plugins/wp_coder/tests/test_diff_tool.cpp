/*
 * test_diff_tool.cpp — И11.2: diff вызова и подсветка «до/после».
 *
 * Предмет проверки — РЕШЕНИЯ, а не окно (как в test_diff_view.cpp, 11.1):
 * тестового харнесса для ImGui нет, и 11.13 будет сверять готовую строку и
 * её вид. Поэтому форма `metadata.filediff`, чтение её и подсветка живут в
 * core/diff.h и проверяются здесь, а ui/coder_window.cpp остаётся тонким
 * слоем поверх.
 *
 * Откуда берутся данные. Три источника, и они не взаимозаменяемы:
 *   - ЖИВЫЕ вызовы инструментов через `ToolsRegistry::run_output` (не
 *     `def.handler`, как требует правило: enforcement живёт в реестре). Это
 *     единственный способ доказать, что патч в метаданных совпадает с
 *     тем, что инструмент реально записал на диск: сверка идёт с
 *     содержимым файла, а не с эталоном, написанным руками;
 *   - `make_file_diff()` для чтения формы — так патч выглядит у снимка;
 *   - патчи и метаданные, набранные руками, — для случаев, которых не
 *     бывает на живых данных: чужой производитель, одиночный объект вместо
 *     списка, запись без имени, патч-мусор. Это единственный способ довести
 *     чтение до состояний, которые нельзя вызвать (то же основание, что у
 *     parse_hash, отклонение 110).
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/diff.h"
#include "../core/engine.h"
#include "../core/json.h"
#include "../core/limits.h"
#include "../core/base_tools.h"
#include "../core/tools_registry.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace coder;

namespace {

fs::path make_tmp_tree() {
    fs::path tmp = fs::temp_directory_path()
        / ("wp_coder_difftool_" + std::to_string(::getpid()) + "_"
           + std::to_string(std::rand()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    return tmp;
}

void put_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

std::string slurp(const fs::path& p) {
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

/* Вызов инструмента с полным результатом: metadata читается у него, а не у
 * обработчика (правило 8 SESSION_START). */
ToolOutput call(const std::string& name, json::JsonValue args) {
    return ToolsRegistry::instance().run_output(name, args);
}

json::JsonValue args_of(std::initializer_list<std::pair<const char*, std::string>> kv) {
    json::JsonValue a = json::JsonValue::object();
    for (const auto& p : kv) a.set(p.first, p.second);
    return a;
}

/* Первая строка такой-то роли. */
const diff::DiffRow* row_of(const std::vector<diff::DiffRow>& rows,
                            diff::DiffRowRole role, size_t n = 0) {
    size_t seen = 0;
    for (const diff::DiffRow& r : rows) {
        if (r.role != role) continue;
        if (seen == n) return &r;
        ++seen;
    }
    return nullptr;
}

size_t index_of(const std::vector<diff::DiffRow>& rows, diff::DiffRowRole role,
                size_t n = 0) {
    size_t seen = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].role != role) continue;
        if (seen == n) return i;
        ++seen;
    }
    return rows.size();
}

const diff::FileDiff* file_named(const std::vector<diff::FileDiff>& files,
                                 const std::string& name) {
    for (const diff::FileDiff& fd : files) {
        if (fd.file == name) return &fd;
    }
    return nullptr;
}

/* Метаданные вызова, прочитанные ТЕМ ЖЕ чтением, каким пользуется окно.
 * Проверка, которая читала бы JSON руками, проверяла бы не то место. */
std::vector<diff::FileDiff> read_back(const ToolOutput& o, std::string* note) {
    std::vector<diff::FileDiff> files;
    diff::filediff_from_metadata(o.metadata, &files, note);
    return files;
}

} // namespace

/* ======================================================================
 * 1. Производитель: что пишущий инструмент кладёт в метаданные
 * ====================================================================== */

TEST(a_written_file_is_reported_as_the_diff_between_what_there_was_and_what_there_is) {
    fs::path t = make_tmp_tree();
    put_file(t / "a.php", "<?php\n$old = 1;\necho 1;\n");
    init_tools(t);

    const ToolOutput o =
        call("write_file", args_of({{"path", "a.php"},
                                     {"content", "<?php\n$new = 2;\necho 1;\n"}}));
    std::string note;
    const std::vector<diff::FileDiff> files = read_back(o, &note);
    ASSERT_EQ(files.size(), (size_t)1);
    ASSERT_EQ(files[0].file, std::string("a.php"));
    ASSERT_EQ(files[0].additions, (size_t)1);
    ASSERT_EQ(files[0].deletions, (size_t)1);

    /* Сверка с ДИСКОМ, а не с эталоном: «до» — то, что лежало до вызова,
     * «после» — то, что записал инструмент. Патч, посчитанный о другом,
     * прошёл бы проверку формы и был бы неправдой. */
    const std::vector<diff::FileDiff> expected = {
        diff::make_file_diff("a.php", "<?php\n$old = 1;\necho 1;\n",
                             slurp(t / "a.php"))};
    ASSERT_EQ(expected[0].patch, files[0].patch);
    ASSERT_EQ(expected[0].additions, files[0].additions);
    ASSERT_EQ(expected[0].deletions, files[0].deletions);

    /* И строка показа: замена, а не «добавлено три строки». */
    const std::vector<diff::DiffRow> rows = diff::parse_unified(files[0].patch);
    const diff::DiffRow* removed = row_of(rows, diff::DiffRowRole::Removed);
    const diff::DiffRow* added = row_of(rows, diff::DiffRowRole::Added);
    ASSERT_TRUE(removed != nullptr);
    ASSERT_TRUE(added != nullptr);
    ASSERT_EQ(removed->text, std::string("$old = 1;"));
    ASSERT_EQ(added->text, std::string("$new = 2;"));
    fs::remove_all(t);
}

TEST(the_four_fields_named_by_the_task_are_written_by_every_producer) {
    /* Строка задачи 11.2 называет форму: {file, patch, additions,
     * deletions}. Проверка идёт по ЖИВЫМ вызовам всех четырёх пишущих
     * инструментов и смотрит сырой JSON, а не результат чтения: иначе
     * «форма одна» проверяла бы саму себя. */
    fs::path t = make_tmp_tree();
    put_file(t / "keep.txt", "one\ntwo\nthree\n");
    init_tools(t);

    std::vector<json::JsonValue> metas;

    /* Каждый вызов обязан РЕАЛЬНО что-то менять: инструмент, которому не
     * нашлось что заменить, возвращает отказ и diff не несёт, и проверка
     * падала бы на отказе, принятом за «производитель молчит». */
    ToolOutput o = call("write_file", args_of({{"path", "keep.txt"},
                                               {"content", "one\nTWO\nthree\n"}}));
    ASSERT_TRUE(o.output.find("[ошибка]") == std::string::npos);
    metas.push_back(o.metadata);
    ASSERT_EQ(slurp(t / "keep.txt"), std::string("one\nTWO\nthree\n"));

    o = call("search_replace", args_of({{"path", "keep.txt"},
                                        {"query", "TWO"}, {"content", "2"}}));
    /* Отказ у search_replace начинается с того же «[search_replace] », что и
     * успех, — различает их «: строка », и проверка на префикс проверила бы
     * ничего. */
    ASSERT_TRUE(o.output.find(": строка ") != std::string::npos);
    metas.push_back(o.metadata);
    ASSERT_EQ(slurp(t / "keep.txt"), std::string("one\n2\nthree\n"));

    o = call("edit_file", args_of({{"path", "keep.txt"}, {"k", "1"},
                                   {"content", "ONE"}}));
    ASSERT_TRUE(o.output.find("[ошибка]") == std::string::npos);
    metas.push_back(o.metadata);
    ASSERT_EQ(slurp(t / "keep.txt"), std::string("ONE\n2\nthree\n"));

    const std::string patch_text =
        "*** Begin Patch\n"
        "*** Update File: keep.txt\n"
        "@@\n"
        "-ONE\n"
        "+uno\n"
        "*** End Patch";
    json::JsonValue pa = json::JsonValue::object();
    pa.set("patchText", patch_text);
    o = call("apply_patch", pa);
    ASSERT_TRUE(o.output.find("[ошибка]") == std::string::npos);
    ASSERT_TRUE(o.output.find("ОТКАЗ") == std::string::npos);
    metas.push_back(o.metadata);
    ASSERT_EQ(slurp(t / "keep.txt"), std::string("uno\n2\nthree\n"));

    for (size_t i = 0; i < metas.size(); ++i) {
        if (metas[i].find("filediff") == nullptr) {
            std::cerr << "  производитель №" << (i + 1)
                      << " не отдал metadata.filediff" << std::endl;
        }
        ASSERT_TRUE(metas[i].find("filediff") != nullptr);
        const json::JsonValue* list = metas[i].find("filediff");
        ASSERT_TRUE(list->is_array());
        ASSERT_TRUE(list->size() >= 1);
        const json::JsonValue& entry = list->at(0);
        ASSERT_TRUE(entry.is_object());
        ASSERT_TRUE(entry.get_string("file") == "keep.txt");
        ASSERT_TRUE(entry.has("patch"));
        ASSERT_TRUE(entry.has("additions"));
        ASSERT_TRUE(entry.has("deletions"));
        /* Счётчики — числа, а не строки: иначе чтение молча взяло бы 0. */
        ASSERT_TRUE(entry.get("additions").is_number());
        ASSERT_TRUE(entry.get("deletions").is_number());
    }
    fs::remove_all(t);
}

TEST(a_replacement_of_one_line_reaches_the_panel_as_a_replacement) {
    /* Конец сквозного пути: правка строки инструментом → блок ЗАМЕНЫ с
     * обеими сторонами. Именно это и называет «до/после», и по роли строки
     * («Removed») отличить её от удаления нельзя. */
    fs::path t = make_tmp_tree();
    put_file(t / "a.txt", "alpha\nbeta\n");
    init_tools(t);
    const ToolOutput o = call("search_replace",
                              args_of({{"path", "a.txt"},
                                       {"query", "beta"}, {"content", "gamma"}}));

    std::string note;
    const std::vector<diff::FileDiff> files = read_back(o, &note);
    ASSERT_EQ(files.size(), (size_t)1);
    const std::vector<diff::DiffRow> rows =
        diff::parse_unified(files[0].patch);
    const std::vector<diff::DiffChangeBlock> blocks = diff::diff_change_blocks(rows);
    ASSERT_EQ(blocks.size(), (size_t)1);
    ASSERT_TRUE(blocks[0].kind == diff::DiffChangeKind::Replacement);
    ASSERT_EQ(blocks[0].before, (size_t)1);
    ASSERT_EQ(blocks[0].after, (size_t)1);

    const size_t removed_at = index_of(rows, diff::DiffRowRole::Removed);
    const size_t added_at = index_of(rows, diff::DiffRowRole::Added);
    ASSERT_TRUE(diff::diff_row_change(blocks, removed_at) ==
                diff::DiffChangeKind::Replacement);
    ASSERT_TRUE(diff::diff_row_change(blocks, added_at) ==
                diff::DiffChangeKind::Replacement);
    /* Подпись блока говорит то же самое словами: у замены две стороны, и
     * одна цифра «удалено 1» была бы ложью. */
    ASSERT_EQ(diff::diff_change_label(blocks[0]), std::string("замена 1 → 1"));
    fs::remove_all(t);
}

TEST(a_created_file_is_reported_as_a_pure_insertion) {
    fs::path t = make_tmp_tree();
    init_tools(t);
    const ToolOutput o = call("write_file",
                              args_of({{"path", "new.txt"},
                                       {"content", "a\nb\nc\n"}}));
    std::string note;
    const std::vector<diff::FileDiff> files = read_back(o, &note);
    ASSERT_EQ(files.size(), (size_t)1);
    ASSERT_EQ(files[0].deletions, (size_t)0);
    const std::vector<diff::DiffRow> rows = diff::parse_unified(files[0].patch);
    const std::vector<diff::DiffChangeBlock> blocks = diff::diff_change_blocks(rows);
    ASSERT_EQ(blocks.size(), (size_t)1);
    ASSERT_TRUE(blocks[0].kind == diff::DiffChangeKind::Insertion);
    ASSERT_EQ(diff::diff_change_label(blocks[0]), std::string("добавлено 3"));
    fs::remove_all(t);
}

TEST(apply_patch_reports_one_entry_per_file_and_a_deleted_one_is_shown_as_deleted) {
    fs::path t = make_tmp_tree();
    put_file(t / "one.txt", "a\nb\n");
    put_file(t / "two.txt", "c\nd\n");
    put_file(t / "three.txt", "e\nf\n");
    init_tools(t);

    /* Один вызов меняет ДВА файла и удаляет третий: контейнер обязан быть
     * списком, иначе правка второго файла потерялась бы (или, что хуже,
     * заняла бы место первого). Каждый файл назван один раз — иначе запись
     * искалась бы по имени и нашла бы не ту (см. про два изменения одного
     * файла в отклонении 148). */
    const std::string patch_text =
        "*** Begin Patch\n"
        "*** Update File: one.txt\n"
        "@@\n"
        "-a\n"
        "+A\n"
        "*** Update File: two.txt\n"
        "@@\n"
        "-c\n"
        "+C\n"
        "*** Delete File: three.txt\n"
        "*** End Patch";
    json::JsonValue pa = json::JsonValue::object();
    pa.set("patchText", patch_text);
    const ToolOutput o = call("apply_patch", pa);
    std::string note;
    const std::vector<diff::FileDiff> files = read_back(o, &note);
    ASSERT_EQ(files.size(), (size_t)3);

    const diff::FileDiff* one = file_named(files, "one.txt");
    ASSERT_TRUE(one != nullptr);
    ASSERT_EQ(one->additions, (size_t)1);
    ASSERT_EQ(one->deletions, (size_t)1);
    const diff::FileDiff* two = file_named(files, "two.txt");
    ASSERT_TRUE(two != nullptr);
    ASSERT_EQ(two->additions, (size_t)1);
    ASSERT_EQ(two->deletions, (size_t)1);
    const diff::FileDiff* three = file_named(files, "three.txt");
    ASSERT_TRUE(three != nullptr);
    /* Удаление — «после» пусто: строки показа должны быть удалёнными, иначе
     * панель нарисовала бы удалённый файл как изменённый. */
    const diff::FileDiff* gone = three;
    ASSERT_EQ(gone->additions, (size_t)0);
    ASSERT_TRUE(gone->deletions >= (size_t)1);
    const std::vector<diff::DiffRow> rows = diff::parse_unified(gone->patch);
    ASSERT_TRUE(row_of(rows, diff::DiffRowRole::Removed) != nullptr);
    ASSERT_TRUE(row_of(rows, diff::DiffRowRole::Added) == nullptr);
    const std::vector<diff::DiffChangeBlock> blocks = diff::diff_change_blocks(rows);
    ASSERT_EQ(blocks.size(), (size_t)1);
    ASSERT_TRUE(blocks[0].kind == diff::DiffChangeKind::Deletion);
    ASSERT_EQ(diff::diff_change_label(blocks[0]),
              std::string("удалено " + std::to_string(blocks[0].before)));
    fs::remove_all(t);
}

TEST(a_file_created_by_apply_patch_is_a_pure_insertion_too) {
    /* Создание через apply_patch проверяется ОТДЕЛЬНО от create через
     * write_file: это другой код (другая ветка патча) и ошибка в нём не
     * видна по проверке соседнего инструмента. Мутация «подставить непустое
     * "до"» именно здесь и выжила бы, не будь этой проверки. */
    fs::path t = make_tmp_tree();
    init_tools(t);
    json::JsonValue pa = json::JsonValue::object();
    pa.set("patchText",
           "*** Begin Patch\n"
           "*** Add File: made.txt\n"
           "+раз\n"
           "+два\n"
           "*** End Patch");
    const ToolOutput o = call("apply_patch", pa);
    ASSERT_TRUE(o.output.find("[ошибко") == std::string::npos);
    ASSERT_EQ(slurp(t / "made.txt"), std::string("раз\nдва\n"));
    std::string note;
    const std::vector<diff::FileDiff> files = read_back(o, &note);
    ASSERT_EQ(files.size(), (size_t)1);
    ASSERT_EQ(files[0].file, std::string("made.txt"));
    ASSERT_EQ(files[0].deletions, (size_t)0);
    ASSERT_EQ(files[0].additions, (size_t)2);
    const std::vector<diff::DiffRow> rows = diff::parse_unified(files[0].patch);
    const std::vector<diff::DiffChangeBlock> blocks = diff::diff_change_blocks(rows);
    ASSERT_EQ(blocks.size(), (size_t)1);
    ASSERT_TRUE(blocks[0].kind == diff::DiffChangeKind::Insertion);
    fs::remove_all(t);
}

TEST(apply_patch_keeps_its_own_metadata_next_to_the_diff) {
    /* Чужие поля метаданных стираться не должны: у apply_patch там свои
     * счётчики и список файлов, и появление diff не даёт им права исчезнуть
     * (иначе 11.4, читающий `files`, потерял бы список). */
    fs::path t = make_tmp_tree();
    put_file(t / "one.txt", "a\n");
    init_tools(t);
    json::JsonValue pa = json::JsonValue::object();
    pa.set("patchText",
           "*** Begin Patch\n"
           "*** Update File: one.txt\n"
           "@@\n"
           "-a\n"
           "+A\n"
           "*** End Patch");
    const ToolOutput o = call("apply_patch", pa);
    ASSERT_TRUE(o.metadata.get_int("updated", 0) == 1);
    ASSERT_TRUE(o.metadata.has("files"));
    ASSERT_TRUE(o.metadata.find("filediff") != nullptr);
    const json::JsonValue* files = o.metadata.find("files");
    ASSERT_TRUE(files != nullptr && files->is_array());
    ASSERT_EQ(files->size(), (size_t)1);
    fs::remove_all(t);
}

TEST(a_moved_file_is_reported_under_the_path_it_now_lives_at) {
    /* Подпись diff — путь, по которому правка ЛЕЖИТ. Со старым именем
     * человек читал бы изменение файла, которого по этому имени уже нет. */
    fs::path t = make_tmp_tree();
    put_file(t / "old.txt", "hello\n");
    init_tools(t);
    json::JsonValue pa = json::JsonValue::object();
    pa.set("patchText",
           "*** Begin Patch\n"
           "*** Update File: old.txt\n"
           "*** Move to: new.txt\n"
           "@@\n"
           "-hello\n"
           "+привет\n"
           "*** End Patch");
    const ToolOutput o = call("apply_patch", pa);
    ASSERT_TRUE(o.output.find("[ошибко") == std::string::npos);
    ASSERT_TRUE(o.output.find("ОТКАЗ") == std::string::npos);
    std::string note;
    const std::vector<diff::FileDiff> files = read_back(o, &note);
    ASSERT_EQ(files.size(), (size_t)1);
    ASSERT_EQ(files[0].file, std::string("new.txt"));
    /* И содержимое на диске совпадает с «после» из патча. */
    ASSERT_EQ(slurp(t / "new.txt"), std::string("привет\n"));
    const std::vector<diff::DiffRow> rows = diff::parse_unified(files[0].patch);
    const diff::DiffRow* added = row_of(rows, diff::DiffRowRole::Added);
    ASSERT_TRUE(added != nullptr);
    ASSERT_EQ(added->text, std::string("привет"));
    fs::remove_all(t);
}

TEST(a_crlf_file_rewritten_with_lf_is_not_reported_as_every_line_changed) {
    /* Формат файла сохраняется при записи (И4.5), и diff обязан считаться по
     * ТОМУ, что уйдёт на диск: иначе перевод строк, добавленный
     * preserve_file_format, показался бы как правка каждой строки — то
     * есть «переписан весь файл» там, где изменена одна строка. */
    fs::path t = make_tmp_tree();
    put_file(t / "win.txt", "a\r\nb\r\n");
    init_tools(t);
    const ToolOutput o = call("write_file",
                              args_of({{"path", "win.txt"},
                                       {"content", "a\nB\n"}}));
    ASSERT_EQ(slurp(t / "win.txt"), std::string("a\r\nB\r\n"));
    std::string note;
    const std::vector<diff::FileDiff> files = read_back(o, &note);
    ASSERT_EQ(files.size(), (size_t)1);
    ASSERT_EQ(files[0].additions, (size_t)1);
    ASSERT_EQ(files[0].deletions, (size_t)1);
    fs::remove_all(t);
}

TEST(a_proposed_write_carries_the_diff_of_what_it_would_do) {
    /* Режим «сначала план»: файл НЕ тронут, а человек обязан видеть, что
     * агент собрался изменить, иначе предложение нечего одобрять. */
    fs::path t = make_tmp_tree();
    put_file(t / "a.txt", "one\ntwo\n");
    init_tools(t);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().plan_mode = true;
    }
    const ToolOutput o = call("search_replace",
                              args_of({{"path", "a.txt"},
                                       {"query", "two"}, {"content", "2"}}));
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().plan_mode = false;
        engine_state().pending.clear();
    }
    std::string note;
    const std::vector<diff::FileDiff> files = read_back(o, &note);
    ASSERT_EQ(files.size(), (size_t)1);
    ASSERT_EQ(files[0].additions, (size_t)1);
    ASSERT_EQ(files[0].deletions, (size_t)1);
    /* Файл на диске не тронут — предложение осталось предложением. */
    ASSERT_EQ(slurp(t / "a.txt"), std::string("one\ntwo\n"));
    fs::remove_all(t);
}

TEST(a_call_that_changed_nothing_carries_no_diff) {
    /* Отказ и «ничего не нашлось» — не правка. Плата за diff, которого не
     * было, — пустой блок в панели и «изменилось 0 строк» там, где
     * человек ничего не менял. */
    fs::path t = make_tmp_tree();
    put_file(t / "a.txt", "one\n");
    init_tools(t);
    const ToolOutput o = call("search_replace",
                              args_of({{"path", "a.txt"},
                                       {"query", "нет такого"}, {"content", "x"}}));
    /* Отказ: сообщение об ошибке и НЕТ ни «: строка », ни diff. */
    ASSERT_TRUE(o.output.find(": строка ") == std::string::npos);
    ASSERT_TRUE(o.output.find("не найдено") != std::string::npos ||
                o.output.find("[search_replace]") != std::string::npos);
    ASSERT_TRUE(o.metadata.find("filediff") == nullptr);
    fs::remove_all(t);
}

TEST(a_binary_file_is_named_instead_of_shown_as_no_changes) {
    /* Двоичный файл — граница 2 шапки core/diff.h: «изменений нет» и
     * «изменился файл, который нечем показать» — разные вещи. */
    fs::path t = make_tmp_tree();
    init_tools(t);
    std::string bin = "head";
    bin.push_back('\0');
    bin += "tail";
    const ToolOutput o = call("write_file",
                              args_of({{"path", "b.bin"}, {"content", bin}}));
    std::string note;
    const std::vector<diff::FileDiff> files = read_back(o, &note);
    ASSERT_EQ(files.size(), (size_t)1);
    ASSERT_TRUE(files[0].binary);
    ASSERT_TRUE(files[0].patch.empty());
    ASSERT_TRUE(files[0].additions == 0);
    ASSERT_TRUE(files[0].deletions == 0);
    ASSERT_TRUE(!files[0].note.empty());
    fs::remove_all(t);
}

/* ======================================================================
 * 2. Чтение: форма, сверка, отказ
 * ====================================================================== */

/* Запись метаданных, собранная из настоящего FileDiff: так выглядит то,
 * что пишет инструмент. */
json::JsonValue meta_of(const std::vector<diff::FileDiff>& files) {
    json::JsonValue m = json::JsonValue::object();
    m.set("filediff", diff::filediff_metadata(files));
    return m;
}

TEST(the_written_form_survives_the_round_trip_unchanged) {
    /* Round-trip: собрали → записали JSON → прочитали. Проверяется не
     * равенство строк, а ЗНАЧЕНИЯ: dump() меняет порядок ключей и пробелы,
     * и сравнение текста проверяло бы сериализатор, а не форму. */
    std::vector<diff::FileDiff> files;
    files.push_back(diff::make_file_diff("a.txt", "a\nb\n", "a\nB\n"));
    json::JsonValue text = meta_of(files);
    json::JsonValue parsed;
    std::string err;
    ASSERT_TRUE(json::JsonValue::parse(text.dump(), parsed, &err));
    std::vector<diff::FileDiff> back;
    std::string note;
    ASSERT_TRUE(diff::filediff_from_metadata(parsed, &back, &note));
    ASSERT_EQ(back.size(), files.size());
    ASSERT_EQ(back[0].file, files[0].file);
    ASSERT_EQ(back[0].patch, files[0].patch);
    ASSERT_EQ(back[0].additions, files[0].additions);
    ASSERT_EQ(back[0].deletions, files[0].deletions);
}

TEST(lying_counters_in_metadata_are_replaced_by_the_patch_and_named) {
    /* Счётчики в метаданных — сверка, а не источник (граница 2 шапки).
     * Человек, читающий панель, должен видеть числа той правки, которую он
     * видит, и узнать, что в метаданных было написано другое. */
    /* Правка выбрана НЕСИММЕТРИЧНОЙ: одна удалённая строка против двух
     * добавленных. На симметричной (1 ↔ 1) перестановка «до» и «после»
     * местами дала бы те же числа, и проверка была бы зелёной на неверном
     * коде — это и случилось бы, будь фикстура симметричной. */
    std::vector<diff::FileDiff> files;
    files.push_back(diff::make_file_diff("a.txt", "a\nb\nc\n", "a\nB\nc\nd\n"));
    json::JsonValue meta = meta_of(files);
    ASSERT_EQ(files[0].additions, (size_t)2);
    ASSERT_EQ(files[0].deletions, (size_t)1);
    const json::JsonValue* list = meta.find("filediff");
    ASSERT_TRUE(list != nullptr);
    /* Портим счётчик: собранный список — массив, элемент копируется и в
     * нём правится одно поле. */
    json::JsonValue holder_list = json::JsonValue::array();
    json::JsonValue entry = list->at(0);
    entry.set("additions", static_cast<long long>(99));
    holder_list.push_back(entry);
    json::JsonValue holder = json::JsonValue::object();
    holder.set("filediff", holder_list);

    std::vector<diff::FileDiff> back;
    std::string note;
    ASSERT_TRUE(diff::filediff_from_metadata(holder, &back, &note));
    ASSERT_EQ(back.size(), (size_t)1);
    ASSERT_TRUE(back[0].additions != (size_t)99);
    ASSERT_EQ(back[0].additions, files[0].additions);
    ASSERT_TRUE(back[0].note.find("не совпали") != std::string::npos);
}

TEST(a_truncated_patch_is_not_accused_of_lying_counters) {
    /* Граница 4 шапки: усечённый патч законно содержит меньше строк, чем
     * счётчики всего файла. Сверка здесь дала бы враньё наоборот —
     * «счётчики не совпали» там, где всё в порядке. */
    /* Усечение НАСТОЯЩЕЕ, а не выставленный флаг: патч режется по
     * limits::kMaxDiffLinesPerFile, и проверка на выдуманном `truncated`
     * проверяла бы только то, что флаг копируется. */
    std::string before, after;
    for (size_t i = 0; i < limits::kMaxDiffLinesPerFile + 200; ++i) {
        before += "было" + std::to_string(i) + "\n";
        after += "стало" + std::to_string(i) + "\n";
    }
    std::vector<diff::FileDiff> files;
    diff::FileDiff fd = diff::make_file_diff("big.txt", before, after);
    ASSERT_TRUE(fd.truncated);   /* основание проверки живое */
    ASSERT_TRUE(fd.note.find("не совпали") == std::string::npos);
    files.push_back(fd);
    std::vector<diff::FileDiff> back;
    std::string note;
    ASSERT_TRUE(diff::filediff_from_metadata(meta_of(files), &back, &note));
    ASSERT_EQ(back.size(), (size_t)1);
    ASSERT_TRUE(back[0].note.find("не совпали") == std::string::npos);
    /* Обрезанный патч остаётся помеченным и объяснённым: иначе панель
     * показала бы срез патча так, будто он весь. */
    ASSERT_TRUE(back[0].truncated);
    ASSERT_TRUE(back[0].note.find("строк патча") != std::string::npos);
    /* Показанные строки меньше записанного счётчика — и это законно. */
    ASSERT_TRUE(back[0].additions < fd.additions);
}

TEST(garbage_in_the_patch_field_is_named_and_not_shown_as_no_changes) {
    /* Поле `patch` пришло из чужого производителя или из повреждённого
     * файла сессии. Показывать из него ноль строк нельзя: человек прочёл бы
     * «прав не было» там, где правка была. */
    json::JsonValue entry = json::JsonValue::object();
    entry.set("file", "a.txt");
    entry.set("patch", "просто текст\nи ещё строка\n");
    entry.set("additions", static_cast<long long>(3));
    entry.set("deletions", static_cast<long long>(1));
    json::JsonValue list = json::JsonValue::array();
    list.push_back(entry);
    json::JsonValue meta = json::JsonValue::object();
    meta.set("filediff", list);

    std::vector<diff::FileDiff> back;
    std::string note;
    ASSERT_TRUE(diff::filediff_from_metadata(meta, &back, &note));
    ASSERT_EQ(back.size(), (size_t)1);
    ASSERT_TRUE(back[0].patch.empty());
    ASSERT_TRUE(back[0].additions == 0);
    ASSERT_TRUE(back[0].note.find("не разобран") != std::string::npos);
    ASSERT_TRUE(note.find("a.txt") != std::string::npos);
}

TEST(a_lone_object_instead_of_a_list_is_refused_by_name) {
    /* «Объект для одного файла, список для нескольких» — та самая вторая
     * форма, которая потом разъезжается. Отказ называет, что прислано, а
     * не молчит: иначе пустая панель выглядела бы как «прав не было». */
    std::vector<diff::FileDiff> files;
    files.push_back(diff::make_file_diff("a.txt", "a\n", "A\n"));
    json::JsonValue lone = diff::filediff_metadata(files).at(0);
    json::JsonValue meta = json::JsonValue::object();
    meta.set("filediff", lone);

    std::vector<diff::FileDiff> back;
    std::string note;
    ASSERT_FALSE(diff::filediff_from_metadata(meta, &back, &note));
    ASSERT_TRUE(back.empty());
    ASSERT_TRUE(note.find("не список") != std::string::npos);
}

TEST(an_entry_without_a_file_name_is_skipped_without_hiding_the_others) {
    /* Один битый файл не должен прятать за собой остальные: человек
     * прочёл бы «прав не было» там, где агент переписал три файла. */
    std::vector<diff::FileDiff> files;
    files.push_back(diff::make_file_diff("a.txt", "a\n", "A\n"));
    files.push_back(diff::make_file_diff("b.txt", "b\n", "B\n"));
    json::JsonValue list = diff::filediff_metadata(files);
    json::JsonValue good_a = list.at(0);
    json::JsonValue nameless = list.at(1);
    nameless.erase("file");
    json::JsonValue holder = json::JsonValue::array();
    holder.push_back(good_a);
    holder.push_back(nameless);
    json::JsonValue meta = json::JsonValue::object();
    meta.set("filediff", holder);

    std::vector<diff::FileDiff> back;
    std::string note;
    ASSERT_TRUE(diff::filediff_from_metadata(meta, &back, &note));
    ASSERT_EQ(back.size(), (size_t)1);
    ASSERT_EQ(back[0].file, std::string("a.txt"));
    ASSERT_TRUE(note.find("без имени файла") != std::string::npos);
}

TEST(metadata_without_filediff_is_not_an_error_but_says_why) {
    /* Инструмент, который файлы не писал, filediff не несёт: это не
     * поломка, и чтение обязано отличать «нечего показывать» от «сломано».
     * Сломано — это filediff не того типа, и это проверяется отдельно. */
    json::JsonValue meta = json::JsonValue::object();
    meta.set("exit_code", static_cast<long long>(0));
    std::vector<diff::FileDiff> back;
    std::string note;
    ASSERT_FALSE(diff::filediff_from_metadata(meta, &back, &note));
    ASSERT_TRUE(back.empty());
    ASSERT_TRUE(note.find("нет metadata.filediff") != std::string::npos);

    /* И не-объект вовсе: чужой читатель не должен на этом упасть. */
    json::JsonValue null_meta;
    ASSERT_FALSE(diff::filediff_from_metadata(null_meta, &back, &note));
    ASSERT_TRUE(note.find("не объект") != std::string::npos);
}

TEST(an_empty_list_is_refused_rather_than_shown_as_a_panel_with_nothing) {
    json::JsonValue meta = json::JsonValue::object();
    meta.set("filediff", json::JsonValue::array());
    std::vector<diff::FileDiff> back;
    std::string note;
    ASSERT_FALSE(diff::filediff_from_metadata(meta, &back, &note));
    ASSERT_TRUE(back.empty());
}

/* ======================================================================
 * 3. Подсветка «до/после»: блоки, полоса, подпись
 * ====================================================================== */

TEST(a_replacement_and_a_deletion_are_told_apart_by_the_block_not_by_the_row) {
    /* Главное свойство подсветки: удалённая строка ЗАМЕНЫ и удалённая
     * строка УДАЛЕНИЯ — одна и та же роль Removed, но подсвечиваться они
     * должны по-разному. Функция, читающая только роль, выродилась бы в
     * переключатель и не отличала бы ничего. */
    const std::string patch =
        "--- a/x\n"
        "+++ b/x\n"
        "@@ -1,3 +1,2 @@\n"
        " ctx\n"
        "-старое\n"
        "+новое\n"
        " ctx2\n"
        "@@ -10,3 +9,2 @@\n"
        " ctx3\n"
        "-удалённое\n"
        " ctx4\n"
        "@@ -20,2 +18,2 @@\n";
    const std::vector<diff::DiffRow> rows = diff::parse_unified(patch);
    const std::vector<diff::DiffChangeBlock> blocks = diff::diff_change_blocks(rows);
    ASSERT_EQ(blocks.size(), (size_t)2);
    ASSERT_TRUE(blocks[0].kind == diff::DiffChangeKind::Replacement);
    ASSERT_EQ(blocks[0].before, (size_t)1);
    ASSERT_EQ(blocks[0].after, (size_t)1);
    ASSERT_TRUE(blocks[1].kind == diff::DiffChangeKind::Deletion);
    ASSERT_EQ(blocks[1].before, (size_t)1);
    ASSERT_EQ(blocks[1].after, (size_t)0);

    const size_t in_replacement = index_of(rows, diff::DiffRowRole::Removed, 0);
    const size_t in_deletion = index_of(rows, diff::DiffRowRole::Removed, 1);
    ASSERT_TRUE(rows[in_replacement].role == rows[in_deletion].role);
    ASSERT_TRUE(diff::diff_row_change(blocks, in_replacement) ==
                diff::DiffChangeKind::Replacement);
    ASSERT_TRUE(diff::diff_row_change(blocks, in_deletion) ==
                diff::DiffChangeKind::Deletion);

    /* Границы блоков — то, за чем окно ищет первую строку подписи. */
    ASSERT_EQ(blocks[0].first, in_replacement);
    ASSERT_EQ(blocks[0].last, in_replacement + 1);
    ASSERT_EQ(blocks[1].first, in_deletion);
    ASSERT_EQ(blocks[1].last, in_deletion);
}

TEST(the_marker_line_of_a_lost_final_newline_stays_inside_its_block) {
    /* `\ No newline at end of file` относится к предыдущей изменённой
     * строке. Разорвать им блок значило бы показать замену как удаление
     * плюс отдельную вставку — то есть две правки там, где была одна. */
    const std::string patch =
        "--- a/x\n"
        "+++ b/x\n"
        "@@ -1,2 +1,2 @@\n"
        " ctx\n"
        "-старое\n"
        "\\ No newline at end of file\n"
        "+новое\n";
    const std::vector<diff::DiffRow> rows = diff::parse_unified(patch);
    const std::vector<diff::DiffChangeBlock> blocks = diff::diff_change_blocks(rows);
    ASSERT_EQ(blocks.size(), (size_t)1);
    ASSERT_TRUE(blocks[0].kind == diff::DiffChangeKind::Replacement);
    const size_t meta_at = index_of(rows, diff::DiffRowRole::Meta, 0);
    ASSERT_TRUE(blocks[0].first <= meta_at);
    ASSERT_TRUE(blocks[0].last >= meta_at);
    /* Вид правки у служебной строки есть — она внутри блока, — а полосы
     * под ней нет: она не текст файла. */
    ASSERT_TRUE(diff::diff_row_change(blocks, meta_at) != diff::DiffChangeKind::None);
    ASSERT_TRUE(diff::diff_change_color(diff::DiffChangeKind::Replacement,
                                        diff::DiffRowRole::Meta).a == 0.0f);
}

TEST(a_new_hunk_begins_a_new_block) {
    /* Две правки в двух hunk'ах — две подписи. Склеенный блок означал бы,
     * что человек правил одно место, а менял два. */
    const std::string patch =
        "--- a/x\n"
        "+++ b/x\n"
        "@@ -1,2 +1,2 @@\n"
        "-одно\n"
        "+одно2\n"
        "@@ -40,2 +40,2 @@\n"
        "-два\n"
        "+два2\n";
    const std::vector<diff::DiffRow> rows = diff::parse_unified(patch);
    const std::vector<diff::DiffChangeBlock> blocks = diff::diff_change_blocks(rows);
    ASSERT_EQ(blocks.size(), (size_t)2);
    for (const diff::DiffChangeBlock& b : blocks) {
        ASSERT_TRUE(b.kind == diff::DiffChangeKind::Replacement);
    }
    ASSERT_EQ(blocks[0].first, index_of(rows, diff::DiffRowRole::Removed, 0));
    ASSERT_EQ(blocks[0].last, index_of(rows, diff::DiffRowRole::Added, 0));
    ASSERT_EQ(blocks[1].first, index_of(rows, diff::DiffRowRole::Removed, 1));
    ASSERT_EQ(blocks[1].last, index_of(rows, diff::DiffRowRole::Added, 1));
    /* Между блоками стоит заголовок второго hunk'а, и он НЕ принадлежит ни
     * одному из них: за ним другая правка, а приклеенный к блоку заголовок
     * красился бы как часть чужой правки. */
    ASSERT_TRUE(blocks[0].last + 1 < blocks[1].first);
    ASSERT_TRUE(diff::diff_row_change(blocks, blocks[0].last + 1) ==
                diff::DiffChangeKind::None);
}

TEST(the_band_of_a_replacement_is_stronger_than_of_one_sided_change) {
    /* В замене обе стороны показаны рядом, и полоса читается как одно
     * изменение; у вставки и удаления второй стороны нет, и полная сила
     * выдавала бы больше, чем показано. */
    const diff::DiffColor both = diff::diff_change_color(
        diff::DiffChangeKind::Replacement, diff::DiffRowRole::Removed);
    const diff::DiffColor only = diff::diff_change_color(
        diff::DiffChangeKind::Deletion, diff::DiffRowRole::Removed);
    const diff::DiffColor after_both = diff::diff_change_color(
        diff::DiffChangeKind::Replacement, diff::DiffRowRole::Added);
    const diff::DiffColor after_only = diff::diff_change_color(
        diff::DiffChangeKind::Insertion, diff::DiffRowRole::Added);

    ASSERT_TRUE(both.a > only.a);
    ASSERT_TRUE(after_both.a > after_only.a);
    ASSERT_TRUE(only.a > 0.0f);
    /* Стороны «до» и «после» различаются ЦВЕТОМ, а не только плотностью:
     * иначе подсветка не сказала бы, к какой стороне строка относится. */
    ASSERT_TRUE(both.r > both.g);
    ASSERT_TRUE(after_both.g > after_both.r);
    /* И полоса полупрозрачная: иначе она закрыла бы текст строки. */
    ASSERT_TRUE(both.a < 1.0f);
    ASSERT_TRUE(only.a < 1.0f);
}

TEST(a_row_outside_a_change_has_no_band_and_an_unknown_index_is_not_a_crash) {
    const std::vector<diff::DiffRow> rows = diff::parse_unified(
        "--- a/x\n"
        "+++ b/x\n"
        "@@ -1,2 +1,2 @@\n"
        " ctx\n"
        "-старое\n"
        "+новое\n");
    const std::vector<diff::DiffChangeBlock> blocks = diff::diff_change_blocks(rows);
    const size_t ctx_at = index_of(rows, diff::DiffRowRole::Context);
    ASSERT_TRUE(diff::diff_row_change(blocks, ctx_at) == diff::DiffChangeKind::None);
    ASSERT_TRUE(diff::diff_change_color(diff::DiffChangeKind::None,
                                        diff::DiffRowRole::Context).a == 0.0f);
    ASSERT_TRUE(diff::diff_change_color(diff::DiffChangeKind::Insertion,
                                        diff::DiffRowRole::Context).a == 0.0f);
    /* Индекс за последней строкой — из окна такое прийти не может, но
     * проверка обязана быть безопасной: упавшее окно хуже пустого. */
    ASSERT_TRUE(diff::diff_row_change(blocks, rows.size() + 10) ==
                diff::DiffChangeKind::None);
    ASSERT_TRUE(diff::diff_row_change(std::vector<diff::DiffChangeBlock>(), 0) ==
                diff::DiffChangeKind::None);
}

TEST(the_label_names_the_kind_and_counts_the_side_it_means) {
    diff::DiffChangeBlock b;
    b.kind = diff::DiffChangeKind::Replacement;
    b.before = 2;
    b.after = 1;
    ASSERT_EQ(diff::diff_change_label(b), std::string("замена 2 → 1"));
    b.kind = diff::DiffChangeKind::Insertion;
    b.before = 0;
    b.after = 3;
    ASSERT_EQ(diff::diff_change_label(b), std::string("добавлено 3"));
    b.kind = diff::DiffChangeKind::Deletion;
    b.before = 4;
    b.after = 0;
    ASSERT_EQ(diff::diff_change_label(b), std::string("удалено 4"));
    /* Подписи без блока не бывает: пустая строка вместо названия чужой
     * правки лучше, чем неверное имя. */
    b.kind = diff::DiffChangeKind::None;
    ASSERT_TRUE(diff::diff_change_label(b).empty());
}

TEST(a_patch_without_changes_has_no_blocks_and_no_band) {
    const std::vector<diff::DiffRow> rows = diff::parse_unified("");
    ASSERT_TRUE(diff::diff_change_blocks(rows).empty());
    ASSERT_TRUE(diff::diff_row_change(diff::diff_change_blocks(rows), 0) ==
                diff::DiffChangeKind::None);
}
