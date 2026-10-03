/*
 * test_snapshot.cpp — И10.1: снимок состояния рабочего каталога.
 *
 * Тут нет ни модели, ни цикла агента: только git и файловая система.
 * Именно поэтому снимок и вынесен в core/snapshot.{h,cpp} отдельно от
 * отката (10.3) и от списка файлов (10.2): иначе проверять его было бы
 * только прогоном агента с сетью, то есть фактически никак.
 *
 * Настоящие репозитории, а не заглушки. Смысл снимка — назвать
 * состояние файлов, и единственный код, который это умеет, — git;
 * подмена `git stash create` заглушкой проверила бы заглушку.
 *
 * Порядок проверок по убыванию цены решения. Первые — про то, КАКОЕ
 * состояние названо (дерево из stash, а не индекс), потому что ошибка
 * там выглядит как рабочий откат: снимок есть, хеш есть, а по нему
 * вернулось бы состояние ДО правки агента.
 */

#include "../core/agent_components.h"
#include "../core/base_tools.h"
#include "../core/shell.h"
#include "../core/snapshot.h"

#include "test_framework.h"

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

using namespace coder;
using namespace coder::snapshot;
namespace fs = std::filesystem;

namespace {

/* Каталог теста. Имя по pid — иначе два теста, идущие подряд, делили бы
 * один каталог, и «снимок взят другим тестом» выглядело бы поломкой
 * снимка.
 *
 * Копирование запрещено, и это не формальность: деструктор удаляет
 * каталог, поэтому копия фикстуры — это отложенная удалка. Лямбда,
 * захватившая такую фикстуру ПО ЗНАЧЕНИЮ, удаляла каталог в конце
 * полного выражения (так и случилось: `cb.path_data_dir = [sb]{…}`
 * стирал проект сразу после создания, и проверка падала с «каталог
 * проекта не найден»). Захватывать такую фикстуру можно только по
 * ссылке. */
struct SandBox {
    fs::path root;
    SandBox(const SandBox&) = delete;
    SandBox& operator=(const SandBox&) = delete;

    explicit SandBox(const char* what) {
        root = fs::temp_directory_path() /
               ("wp_coder_snap_" + std::string(what) + "_" +
                std::to_string(::getpid()));
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / "project", ec);
        fs::create_directories(root / "store", ec);
    }
    ~SandBox() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    std::string project() const { return (root / "project").string(); }
    std::string store() const { return (root / "store").string(); }
};

void write_file(const fs::path& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
}

std::string read_text(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

/* Вывод команды без служебных обёрток плагина: git нужен тут как
 * ЭТАЛОН для сравнения, а проверка не должна зависеть от того, через
 * что именно плагин с ним ходит (иначе одинаково неверная обёртка
 * сравнивала бы сама себя). */
std::string raw_git(const std::string& dir, const std::string& args) {
    std::string out;
    int code = 0;
    shell::run_capture_status("git -C " + shell::shell_quote(dir) + " " + args,
                              out, code, 20);
    return out;
}

/* Первая строка вывода длиной не меньше 40 символов: так берутся
 * хеши git и имя ветки. Порог «не меньше 40» вместо проверки на hex —
 * чтобы проверка не повторяла ту же функцию, что и код (иначе ошибка в
 * разборе была бы у них общая и обе проверки стали бы зелёными). */
std::string first_hash(const std::string& text) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        const std::string line =
            text.substr(pos, nl == std::string::npos ? std::string::npos
                                                     : nl - pos);
        if (line.size() >= 40) return line;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return "";
}

/* Репозиторий с одним коммитом: автора и почту задаём, потому что
 * `git commit` без них падает, а падение выглядело бы поломкой
 * снимка. */
void init_repo(const std::string& dir) {
    raw_git(dir, "init -q .");
    raw_git(dir, "config user.email test@example.com");
    raw_git(dir, "config user.name test");
    raw_git(dir, "config commit.gpgsign false");
}

/* Репозиторий с непустым состоянием и одним коммитом. */
void init_repo_with_file(const std::string& dir, const std::string& name,
                         const std::string& text) {
    init_repo(dir);
    write_file(fs::path(dir) / name, text);
    raw_git(dir, "add -A");
    raw_git(dir, "commit -q -m init");
}

/* Содержимое файла ВНУТРИ дерева (tree-hash — это дерево, а не путь
 * в рабочем дереве: по нему нельзя ни сходить `cat`, ни открыть файл). */
std::string blob_in_tree(const std::string& dir, const std::string& tree,
                         const std::string& name) {
    return raw_git(dir, "cat-file -p " + shell::shell_quote(tree + ":" + name));
}

} // namespace

/* ======================================================================
 * Каталог копий и признак репозитория
 * ====================================================================== */

TEST(snapshot_store_dir_lives_under_the_plugin_data_dir) {
    /* Пустой data_dir — хранить некуда, и это должно быть видно по
     * пустой строке, а не по каталогу в текущем рабочем каталоге. */
    ASSERT_EQ(store_dir(""), std::string(""));
    ASSERT_EQ(store_dir("/tmp/data"), std::string("/tmp/data/wp_coder/snapshots"));
}

TEST(a_plain_directory_is_not_a_git_repo) {
    SandBox sb("repo_flag");
    ASSERT_FALSE(is_git_repo(sb.project()));
    /* Не задан каталог — тоже не репозиторий: «репозиторий неизвестен»
     * и «репозитория нет» отличаются только в ветке, которая и должна
     * быть единственной. */
    ASSERT_FALSE(is_git_repo(""));
    ASSERT_FALSE(is_git_repo(sb.root.string() + "/нет-такого"));
    init_repo_with_file(sb.project(), "a.txt", "one\n");
    ASSERT_TRUE(is_git_repo(sb.project()));
}

/* ======================================================================
 * Git: какое состояние названо снимком
 * ====================================================================== */

TEST(a_snapshot_of_a_clean_repo_names_the_committed_tree) {
    SandBox sb("clean");
    init_repo_with_file(sb.project(), "a.txt", "one\n");
    const std::string head_tree =
        first_hash(raw_git(sb.project(), "rev-parse HEAD^{tree}"));
    ASSERT_EQ(head_tree.size(), (size_t)40);

    const Snapshot s = take(sb.project(), sb.store());
    ASSERT_TRUE(s.ok());
    ASSERT_TRUE(s.kind == Kind::GitTree);
    /* Ровно дерево HEAD: стэшить нечего, и взят индекс — он и есть
     * текущее состояние. */
    ASSERT_EQ(s.hash, head_tree);
    ASSERT_EQ(s.dir, std::string(""));
}

TEST(a_snapshot_carries_the_text_the_agent_just_wrote) {
    SandBox sb("dirty");
    init_repo_with_file(sb.project(), "a.txt", "one\n");
    /* Правка агента в рабочем дереве, в индексе её ещё нет — именно
     * этот случай отличает дерево из stash create от индекса. */
    write_file(fs::path(sb.project()) / "a.txt", "two\n");

    const Snapshot s = take(sb.project(), sb.store());
    ASSERT_TRUE(s.kind == Kind::GitTree);
    ASSERT_EQ(blob_in_tree(sb.project(), s.hash, "a.txt"),
              std::string("two\n"));
    /* Индекс по-прежнему называет старое состояние: если бы снимок был
     * взят из него, откат вернул бы «one». */
    const std::string index_tree =
        first_hash(raw_git(sb.project(), "write-tree"));
    ASSERT_EQ(blob_in_tree(sb.project(), index_tree, "a.txt"),
              std::string("one\n"));
}

TEST(taking_a_snapshot_does_not_touch_the_working_tree) {
    SandBox sb("untouched");
    init_repo_with_file(sb.project(), "a.txt", "one\n");
    write_file(fs::path(sb.project()) / "a.txt", "two\n");
    const std::string before = raw_git(sb.project(), "status --short");
    const std::string stashes_before = raw_git(sb.project(), "stash list");

    const Snapshot s = take(sb.project(), sb.store());
    ASSERT_TRUE(s.ok());
    ASSERT_EQ(raw_git(sb.project(), "status --short"), before);
    /* Ключевое: `git stash create` создаёт объект, но НЕ кладёт его в
     * список stash. Если бы он клал, у человека на руках остался бы
     * «снимок», который не снимает ничего, и список рос бы с каждым
     * шагом агента. */
    ASSERT_EQ(raw_git(sb.project(), "stash list"), stashes_before);
}

TEST(a_repo_without_a_first_commit_still_gets_a_snapshot) {
    /* `git init` без первого коммита: `git stash create` на этом падает
     * («нет начального коммита»), и если бы снимок держался только на
     * нём, свежий каталог остался бы без снимка — а это самый частый
     * случай у человека, который начал проект и ещё не закоммитил. */
    SandBox sb("empty_repo");
    init_repo(sb.project());
    write_file(fs::path(sb.project()) / "a.txt", "one\n");

    const Snapshot s = take(sb.project(), sb.store());
    ASSERT_TRUE(s.kind == Kind::GitTree);
    ASSERT_TRUE(s.hash.size() >= 40);
    /* Пустое дерево git — это законное состояние, а не мусор. */
    ASSERT_TRUE(fs::is_empty(sb.store()));
}

TEST(a_conflicted_repository_says_why_it_has_no_snapshot) {
    /* Конфликт в индексе: ни stash create, ни write-tree не могут
     * назвать состояние. Молча пропустить снимок нельзя — откат (10.3)
     * сказал бы «снимок есть», а состояния нет. */
    SandBox sb("conflict");
    init_repo(sb.project());
    write_file(fs::path(sb.project()) / "a.txt", "base\n");
    raw_git(sb.project(), "add -A");
    raw_git(sb.project(), "commit -q -m base");

    raw_git(sb.project(), "checkout -q -b other");
    write_file(fs::path(sb.project()) / "a.txt", "from-other\n");
    raw_git(sb.project(), "commit -q -am other");
    /* Идентификатор берём ЗДЕСЬ, уже на other: rev-parse HEAD до
     * переключения назвал бы общий предок, и merge сказал бы «уже
     * актуально» — то есть конфликта не было бы, а проверка прошла бы
     * на чистом дереве. */
    const std::string other =
        first_hash(raw_git(sb.project(), "rev-parse HEAD"));
    raw_git(sb.project(), "checkout -q -");
    write_file(fs::path(sb.project()) / "a.txt", "from-main\n");
    raw_git(sb.project(), "commit -q -am main");
    raw_git(sb.project(), "merge " + shell::shell_quote(other));

    const Snapshot s = take(sb.project(), sb.store());
    ASSERT_TRUE(s.kind == Kind::None);
    ASSERT_TRUE(s.reason.size() > 0);
    ASSERT_EQ(s.hash, std::string(""));
}

/* ======================================================================
 * Не git: копия каталога
 * ====================================================================== */

TEST(a_hash_is_taken_out_of_git_output_and_nowhere_else) {
    /* Команда идёт через `2>&1`, то есть stdout может начаться с
     * предупреждения git. «Первая непустая строка» была бы им, и в
     * Snapshot::hash попал бы текст — а 10.3 подставила бы его в команду
     * восстановления. Проверяется на тексте: заставить git напечатать
     * предупреждение и вернуть код 0 нельзя детерминированно. */
    const std::string h = "9464c8a7e8b1893d04f60c97faa79305da50667a";
    ASSERT_EQ(parse_hash(h + "\n"), h);
    ASSERT_EQ(parse_hash(""), std::string(""));
    ASSERT_EQ(parse_hash("fatal: не найден репозиторий\n"), std::string(""));
    ASSERT_EQ(parse_hash("hint: образец\n" + h + "\n"), h);
    /* Пропуск пустых строк: у git вывод заканчивается переводом строки,
     * и пустая строка не должна обрывать разбор. */
    ASSERT_EQ(parse_hash("\n\n" + h + "\n"), h);
    /* 39 символов — ещё не хеш, 40 — уже. Граница закреплена, потому что
     * именно на ней «строка достаточной длины» превращается в хеш. */
    ASSERT_EQ(parse_hash(std::string(39, 'a') + "\n"), std::string(""));
    ASSERT_EQ(parse_hash(std::string(40, 'a') + "\n"), std::string(40, 'a'));
    /* Хеш длиннее 64 (sha256 — 64, а не «сколько вышло») — не наш: в
     * него нечто-то подмешалось. */
    ASSERT_EQ(parse_hash(std::string(65, 'a') + "\n"), std::string(""));
    /* Прописные — не hex в нашей форме: git их не печатает, а значит
     * строка с прописными пришла из другого источника. */
    ASSERT_EQ(parse_hash("9464C8A7E8B1893D04F60C97FAA79305DA50667A\n"),
              std::string(""));
}

TEST(a_project_without_git_is_copied_whole) {
    SandBox sb("copy");
    write_file(fs::path(sb.project()) / "a.txt", "one\n");
    std::error_code ec;
    fs::create_directories(fs::path(sb.project()) / "wp-content");
    write_file(fs::path(sb.project()) / "wp-content" / "style.css", "body{}\n");

    const Snapshot s = take(sb.project(), sb.store());
    ASSERT_TRUE(s.kind == Kind::DirCopy);
    ASSERT_TRUE(s.hash.size() > 0);
    ASSERT_EQ(s.dir, sb.store() + "/" + s.hash);
    /* Содержимое совпадает побайтно, иначе откат вернул бы не то. */
    ASSERT_EQ(read_text(fs::path(s.dir) / "wp-content" / "style.css"),
              std::string("body{}\n"));
    ASSERT_EQ(read_text(fs::path(s.dir) / "a.txt"), std::string("one\n"));
}

TEST(a_copy_does_not_contain_the_previous_copies) {
    /* Каталог копий внутри проекта — так устроена фикстура тестов
     * цикла. Без исключения копия содержала бы предыдущие копии, и
     * каждая следующая была бы вдвое больше: на 12 шагах это
     * копия копии копии, а не откат. */
    SandBox sb("inside");
    write_file(fs::path(sb.project()) / "a.txt", "one\n");
    const std::string store = store_dir(sb.project());
    std::error_code ec;
    fs::create_directories(store, ec);

    const Snapshot first = take(sb.project(), store);
    ASSERT_TRUE(first.kind == Kind::DirCopy);
    write_file(fs::path(sb.project()) / "a.txt", "two\n");
    const Snapshot second = take(sb.project(), store);
    ASSERT_TRUE(second.kind == Kind::DirCopy);
    ASSERT_TRUE(second.hash != first.hash);

    /* Второй снимок содержит файлы проекта, но не каталог копий. */
    ASSERT_EQ(read_text(fs::path(second.dir) / "a.txt"), std::string("two\n"));
    ASSERT_FALSE(fs::exists(fs::path(second.dir) / "wp_coder" / "snapshots"));
    /* И размер остался размером проекта, а не проекта с копиями. */
    size_t files_in_copy = 0;
    std::error_code walk_ec;
    for (fs::recursive_directory_iterator it(fs::path(second.dir), walk_ec),
         end;
         it != end; it.increment(walk_ec)) {
        (void)it;
        ++files_in_copy;
    }
    ASSERT_EQ(files_in_copy, (size_t)1);
}

TEST(a_symlink_is_not_copied) {
    /* Ссылка на каталог дала бы бесконечный обход, а копирование цели
     * разорвало бы саму ссылку. Пропуск — единственный честный выход,
     * и он обязан быть назван: снимок без ссылок неполон. */
    SandBox sb("symlink");
    write_file(fs::path(sb.project()) / "a.txt", "one\n");
    std::error_code ec;
    fs::create_directory_symlink(fs::path(sb.project()),
                                 fs::path(sb.project()) / "self");

    const Snapshot s = take(sb.project(), sb.store());
    ASSERT_TRUE(s.kind == Kind::DirCopy);
    ASSERT_FALSE(fs::exists(fs::path(s.dir) / "self"));
    ASSERT_TRUE(fs::exists(fs::path(s.dir) / "a.txt"));
}

TEST(a_copy_needs_somewhere_to_live) {
    SandBox sb("nowhere");
    write_file(fs::path(sb.project()) / "a.txt", "one\n");
    const Snapshot s = take(sb.project(), "");
    ASSERT_TRUE(s.kind == Kind::None);
    ASSERT_TRUE(s.reason.find("каталог данных") != std::string::npos);
}

TEST(a_missing_project_is_not_a_snapshot) {
    SandBox sb("missing");
    const Snapshot no_dir = take("", sb.store());
    ASSERT_TRUE(no_dir.kind == Kind::None);
    ASSERT_TRUE(no_dir.reason.size() > 0);

    const Snapshot no_such = take(sb.root.string() + "/нет-такого", sb.store());
    ASSERT_TRUE(no_such.kind == Kind::None);
    ASSERT_TRUE(no_such.reason.find("не найден") != std::string::npos);
}

/* ======================================================================
 * Точка подключения: снимок кладётся в состояние движка
 * ====================================================================== */

TEST(a_step_snapshot_lands_in_the_engine_state) {
    SandBox sb("step");
    init_repo_with_file(sb.project(), "a.txt", "one\n");
    EngineState& state = engine_state();
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        state.project_dir = sb.project();
        state.last_snapshot = Snapshot();
        state.snapshots_taken = 0;
    }
    HostCallbacks cb;
    cb.path_data_dir = [&sb] { return sb.store(); };

    /* Первый шаг: в состоянии до вызова снимка не было, и это видно
     * именно в возврате — читать состояние ПОСЛЕ вызова уже поздно,
     * там лежит новый снимок (подробно — в agent_components.h). */
    const StepSnapshot first = take_step_snapshot(state, cb, nullptr);
    ASSERT_TRUE(first.after.ok());
    ASSERT_FALSE(first.before.ok());
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        ASSERT_TRUE(state.last_snapshot.ok());
        ASSERT_EQ(state.last_snapshot.hash, first.after.hash);
        ASSERT_EQ(state.snapshots_taken, 1);
    }
    /* Второй шаг: `before` обязан быть ПЕРВЫМ снимком, а не вторым —
     * иначе сравнивать было бы нечего. Первая версия подключения
     * читала состояние после вызова, получала «до и после совпали» и
     * объявляла, что агент ничего не менял; поймала эта проверка. */
    /* Между шагами файл меняется — иначе два чистых дерева совпали бы
     * и равенство хешей ничего бы не доказывало. */
    write_file(fs::path(sb.project()) / "a.txt", "two\n");
    const StepSnapshot second = take_step_snapshot(state, cb, nullptr);
    ASSERT_TRUE(second.before.ok());
    ASSERT_EQ(second.before.hash, first.after.hash);
    ASSERT_TRUE(second.after.ok());
    ASSERT_TRUE(second.before.hash != second.after.hash);
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        ASSERT_EQ(state.snapshots_taken, 2);
    }
    /* Синглтон не оставляет за собой чужого проекта. */
    std::lock_guard<std::mutex> lk(state.mtx);
    state.project_dir.clear();
    state.last_snapshot = Snapshot();
}

TEST(a_step_snapshot_reports_a_failure_once_per_reason) {
    /* Отказ git на каждом шаге засыпал бы ленту событий одинаковыми
     * строками, и человек, читающий её, пропустил бы всё остальное.
     * Молчание, наоборот, означало бы «откат работает», когда он не
     * работает. Повтор — только когда причина ИЗМЕНИЛАСЬ. */
    SandBox sb("report");
    EngineState& state = engine_state();
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        state.project_dir = sb.root.string() + "/нет-такого";
        state.last_snapshot = Snapshot();
    }
    HostCallbacks cb;
    cb.path_data_dir = [&sb] { return sb.store(); };
    std::vector<std::string> events;
    auto collect = [&events](AgentEvent::Kind k, const std::string& text) {
        if (k == AgentEvent::Status) events.push_back(text);
    };

    take_step_snapshot(state, cb, collect);
    take_step_snapshot(state, cb, collect);
    ASSERT_EQ(events.size(), (size_t)1);
    ASSERT_TRUE(events[0].find("Снимок") != std::string::npos);
    /* Причина сменилась — сообщение обязано появиться снова. */
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        state.project_dir.clear();
        state.last_snapshot = Snapshot();
    }
    events.clear();
    take_step_snapshot(state, cb, collect);
    ASSERT_EQ(events.size(), (size_t)0);

    std::lock_guard<std::mutex> lk(state.mtx);
    state.last_snapshot = Snapshot();
}
TEST(the_subagent_turn_snapshots_the_workspace_too) {
    /* У субагента СВОЙ цикл (run_subagent_turn — функция, а не
     * AgentLoop), и без своей точки подключения автор WP-правок (8.15)
     * оставлял бы файлы без снимка: «откат работает» было бы верно
     * только для агента сессии. Проверяется именно это — прямой вызов
     * хода ребёнка, без родительского цикла, который снимки берёт сам и
     * замаскировал бы пропуск. */
    SandBox sb("child");
    write_file(fs::path(sb.project()) / "a.txt", "one\n");
    EngineState& state = engine_state();
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        state.project_dir = sb.project();
        state.last_snapshot = Snapshot();
        state.snapshots_taken = 0;
        state.abort_requested.store(false);
    }
    HostCallbacks cb;
    cb.path_data_dir = [&sb] { return sb.store(); };
    cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&,
                     LlmReply& out) {
        out.content = "Итог работы субагента.";
        out.finish_reason = "stop";
        out.prompt_tokens = 10;
        out.completion_tokens = 5;
        return true;
    };

    const SubagentResult r =
        run_subagent_turn(state, cb, nullptr, "системный промпт", "задача", 4);
    ASSERT_TRUE(r.ok);
    /* Ответ без вызовов инструментов закрывает ход за один шаг — то
     * есть ровно один снимок. Если бы шагов вышло больше, проверка
     * «снимок на каждом шаге» превратилась бы в «снимок когда-нибудь». */
    ASSERT_EQ(r.steps, 1);
    ASSERT_EQ(engine_state().snapshots_taken, 1);
    ASSERT_TRUE(engine_state().last_snapshot.ok());

    std::lock_guard<std::mutex> lk(state.mtx);
    state.project_dir.clear();
    state.last_snapshot = Snapshot();
}

/* ======================================================================
 * И10.2: что изменилось между двумя снимками
 * ====================================================================== */

TEST(diff_dirs_sees_a_changed_an_added_and_a_removed_file) {
    /* Каталоги сравниваются побайтно, и все три исхода различимы: без
     * «удалённого» список выглядел бы полным, а откат (10.3) вернул бы
     * файл, которого человек удалил сам. */
    SandBox sb("diffdirs");
    const fs::path before = fs::path(sb.root) / "before";
    const fs::path after = fs::path(sb.root) / "after";
    std::error_code ec;
    fs::create_directories(before / "вложенный", ec);
    fs::create_directories(after / "вложенный", ec);
    write_file(before / "a.txt", "one\n");
    write_file(before / "b.txt", "уходит\n");
    write_file(before / "вложенный" / "c.txt", "глубоко\n");
    write_file(after / "a.txt", "two\n");                       /* изменён */
    write_file(after / "вложенный" / "c.txt", "глубоко\n");     /* не тронут */
    write_file(after / "d.txt", "новый\n");                     /* добавлен */

    const std::vector<std::string> files =
        diff_dirs(before.string(), after.string());
    ASSERT_EQ(files.size(), (size_t)3);
    ASSERT_EQ(files[0], std::string("a.txt"));
    ASSERT_EQ(files[1], std::string("b.txt"));
    ASSERT_EQ(files[2], std::string("d.txt"));
    /* Порядок задан, а не «как обошёл каталог»: список попадает в файл
     * сессии, и та же работа обязана выглядеть одинаково. */
    ASSERT_TRUE(files[0] < files[1] && files[1] < files[2]);
}

TEST(diff_dirs_ignores_a_symlink_and_a_missing_directory) {
    SandBox sb("diffsymlink");
    const fs::path before = fs::path(sb.root) / "before";
    const fs::path after = fs::path(sb.root) / "after";
    std::error_code ec;
    fs::create_directories(before, ec);
    fs::create_directories(after, ec);
    write_file(after / "a.txt", "one\n");
    fs::create_directory_symlink(after, after / "назад");

    /* Ссылка на каталог не разворачивается в обход: `назад` ведёт в
     * сам after, и обход ушёл бы в бесконечность. */
    const std::vector<std::string> files =
        diff_dirs(before.string(), after.string());
    ASSERT_EQ(files.size(), (size_t)1);
    ASSERT_EQ(files[0], std::string("a.txt"));

    /* Ссылка на ФАЙЛ — отдельный случай, и он ловится отдельно: по
     * ссылке статус файла такой же, как у цели, то есть без явной
     * проверки она считалась бы обычным файлом и её содержимое молча
     * сравнивалось бы вместо содержимого цели. Своя пара каталогов:
     * в первой a.txt отсутствует, и он попал бы в список, замазав
     * различие между «новый файл» и «новый симлинк». */
    const fs::path b2 = fs::path(sb.root) / "before2";
    const fs::path a2 = fs::path(sb.root) / "after2";
    fs::create_directories(b2, ec);
    fs::create_directories(a2, ec);
    write_file(b2 / "цель.txt", "другое\n");
    write_file(a2 / "цель.txt", "данные\n");
    fs::create_symlink(a2 / "цель.txt", a2 / "ссылка.txt");

    const std::vector<std::string> with_link = diff_dirs(b2.string(), a2.string());
    ASSERT_EQ(with_link.size(), (size_t)1);
    ASSERT_EQ(with_link[0], std::string("цель.txt"));
    /* Несуществующий каталог — пустой список, а не исключение: вызывающий
     * сам различает «нечего сравнивать» по ok. */
    ASSERT_EQ(diff_dirs(sb.root.string() + "/нет-такого",
                        after.string()).size(), (size_t)0);
}

TEST(changed_files_of_two_git_snapshots_lists_what_the_agent_touched) {
    SandBox sb("gitdiff");
    init_repo(sb.project());
    write_file(fs::path(sb.project()) / "a.txt", "one\n");
    write_file(fs::path(sb.project()) / "уходит.txt", "было\n");
    /* Имя с не-ASCII — не украшение: без `-z` git печатает его
     * восьмеричными побегами в кавычках, и в файл сессии ушёл бы не
     * путь, а его текстовое представление. Имя обязано быть
     * ОТСЛЕЖИВАЕМЫМ: неотслеживаемые файлы не видны ни снимку, ни
     * diff, и это ограничение наследуется (см. шапку snapshot.h). */
    write_file(fs::path(sb.project()) / "правка-ü.txt", "прежде\n");
    raw_git(sb.project(), "add -A");
    raw_git(sb.project(), "commit -q -m base");

    const Snapshot before = take(sb.project(), sb.store());
    ASSERT_TRUE(before.ok());

    write_file(fs::path(sb.project()) / "a.txt", "two\n");
    write_file(fs::path(sb.project()) / "правка-ü.txt", "стало\n");
    fs::remove(fs::path(sb.project()) / "уходит.txt");
    write_file(fs::path(sb.project()) / "новый.txt", "неотслеживаемый\n");

    const Snapshot after = take(sb.project(), sb.store());
    const FileChanges ch = changed_files(before, after);
    ASSERT_TRUE(ch.ok);
    ASSERT_EQ(ch.files.size(), (size_t)3);
    ASSERT_EQ(ch.files[0], std::string("a.txt"));
    ASSERT_EQ(ch.files[1], std::string("правка-ü.txt"));
    ASSERT_EQ(ch.files[2], std::string("уходит.txt"));
    for (const std::string& f : ch.files) {
        /* Побегов быть не должно: путь приходит сырым. */
        ASSERT_TRUE(f.find('\\') == std::string::npos);
    }
}

TEST(an_unchanged_project_gives_an_empty_list_and_a_failed_snapshot_says_why) {
    /* «Ничего не изменилось» и «сравнивать нечем» — разные факты, и
     * одним пустым списком их не различить: часть «ход ничего не
     * менял» и часть «сравнение не состоялось» выглядели бы одинаково,
     * а 10.3 откатывал бы по несуществующему списку. */
    SandBox sb("unchanged");
    init_repo_with_file(sb.project(), "a.txt", "one\n");
    const Snapshot s1 = take(sb.project(), sb.store());
    const Snapshot s2 = take(sb.project(), sb.store());

    const FileChanges same = changed_files(s1, s2);
    ASSERT_TRUE(same.ok);
    ASSERT_EQ(same.files.size(), (size_t)0);

    Snapshot broken;
    broken.reason = "каталог проекта не найден";
    const FileChanges against_broken = changed_files(s1, broken);
    ASSERT_FALSE(against_broken.ok);
    ASSERT_TRUE(against_broken.reason.find("не найден") != std::string::npos);

    /* Снимки разных видов сравнивать нельзя: у дерева и у копии каталога
     * разные единицы (путь от корня против пути от проекта). */
    Snapshot copy = s1;
    copy.kind = Kind::DirCopy;
    const FileChanges mixed = changed_files(s1, copy);
    ASSERT_FALSE(mixed.ok);
    ASSERT_TRUE(mixed.reason.find("разных видов") != std::string::npos);
}

TEST(attach_patch_part_lands_in_the_turn_that_made_the_changes) {
    /* Часть кладётся в ПОСЛЕДНЕЕ сообщение ассистента: изменения
     * принадлежат тому ходу, чьи инструменты их сделали. Приписанная к
     * чужой реплике, она означала бы «это изменилось, пока я говорил». */
    std::vector<Message> history;
    history.push_back(Message::user("сделай"));
    Message first = Message::assistant("первый ход");
    first.parts.push_back(MessagePart::text("работаю"));
    history.push_back(first);
    history.push_back(Message::user("уточнение"));
    Message second = Message::assistant("второй ход");
    history.push_back(second);

    FileChanges ch;
    ch.ok = true;
    ch.files = {"a.php", "b.php"};
    Snapshot before;
    before.kind = Kind::GitTree;
    before.hash = "abc123";

    Message* target = attach_patch_part(history, before, ch);
    ASSERT_TRUE(target != nullptr);
    ASSERT_EQ(target->id, second.id);
    /* Часть легла в СООБЩЕНИЕ ВЕКТОРА, а не в локальную копию: второй
     * экземпляр `second` остался бы без части, и проверка на нём ничего
     * бы не говорила о коде. */
    Message* stored = find_message(history, second.id);
    ASSERT_TRUE(stored != nullptr);
    ASSERT_EQ(stored->parts.size(), (size_t)1);
    ASSERT_TRUE(stored->parts[0].is(PartKind::Patch));
    /* Хеш — тот, С КОГО считали: вернуться надо к состоянию ДО
     * изменений этого хода, а не к тому, что получилось. */
    ASSERT_EQ(stored->parts[0].snapshot_hash(), std::string("abc123"));
    ASSERT_EQ(stored->parts[0].files().size(), (size_t)2);
    ASSERT_EQ(stored->parts[0].files().at(0).as_string(), std::string("a.php"));

    /* Пустой список — это «ход ничего не менял», и такую правку хранить
     * незачем: часть без содержимого только размножала бы записи
     * сессии. Несравнимые снимки — другой повод, и тоже без части. */
    FileChanges empty;
    empty.ok = true;
    FileChanges failed;
    failed.reason = "сравнивать нечего";
    ASSERT_TRUE(attach_patch_part(history, before, empty) == nullptr);
    ASSERT_TRUE(attach_patch_part(history, before, failed) == nullptr);
    ASSERT_EQ(find_message(history, second.id)->parts.size(), (size_t)1);

    /* Ассистентских сообщений нет — класть некуда, и это не ошибка:
     * первый шаг цикла сравнивает снимки, когда история ещё пуста. */
    std::vector<Message> fresh;
    fresh.push_back(Message::user("сделай"));
    ASSERT_TRUE(attach_patch_part(fresh, before, ch) == nullptr);
}

TEST(a_nul_separated_command_output_survives_the_shell_wrapper) {
    /* Обёртка shell читала вывод команды как строку (`out += buf`) и
     * обрывала его на первом NUL. Для текста это незаметно, а
     * `git diff --name-only -z` отдаёт первым путём весь ответ и молча
     * теряет остальные: список изменившихся файлов выходил неполным,
     * и откат 10.3 вернул бы не всё. Проверяется не git и не снимки, а
     * обёртка — иначе поломка ждала бы своего симптома. */
    std::string out;
    int code = -1;
    const bool ok =
        shell::run_capture_status("printf 'a\\0b\\0c\\0'", out, code, 5);
    ASSERT_TRUE(ok);
    ASSERT_EQ(out, std::string("a\0b\0c\0", 6));
}

TEST(changed_files_names_paths_from_the_project_and_not_from_the_repository) {
    /* Проект плагина — возможно ПОДКАТАЛОГ репозитория (монорепозиторий,
     * а каталог данных рядом с ним). git печатает пути от корня
     * репозитория, и путь «wp-content/deep/z.php» из корня для агента
     * не значил бы ничего: он оперирует путями от project_dir. Без
     * --relative проверка этого не увидела бы — на проекте, который сам
     * является корнем репозитория, обе формы совпадают. */
    SandBox sb("relative");
    const fs::path repo = fs::path(sb.root) / "repo";
    const fs::path app = repo / "wp-content";
    std::error_code ec;
    fs::create_directories(app / "deep", ec);
    write_file(repo / "снаружи.txt", "не наш\\n");
    write_file(app / "deep" / "z.php", "<?php\\n");
    init_repo(repo.string());
    raw_git(repo.string(), "add -A");
    raw_git(repo.string(), "commit -q -m base");

    const Snapshot before = take(app.string(), sb.store());
    ASSERT_TRUE(before.kind == Kind::GitTree);
    write_file(app / "deep" / "z.php", "<?php // правка\\n");

    const Snapshot after = take(app.string(), sb.store());
    const FileChanges ch = changed_files(before, after);
    ASSERT_TRUE(ch.ok);
    ASSERT_EQ(ch.files.size(), (size_t)1);
    ASSERT_EQ(ch.files[0], std::string("deep/z.php"));
    /* Чужой файл вне проекта в список не попадает: он не часть работы
     * агента, и 10.3 не должен трогать то, что лежит рядом. */
    ASSERT_TRUE(ch.files[0].find("снаружи") == std::string::npos);
}

/* ======================================================================
 * И10.3: возврат к состоянию снимка
 *
 * Порядок — по цене решения. Первые проверки: ЧТО вернулось и ЧТО
 * осталось нетронутым. Обе ловятся только на живом git и на живой
 * файловой системе, и обе смотрели бы вхолостую на заглушке: откат,
 * который «восстановил» бы всё, и откат, который снёс бы лишнее, —
 * это два разных дефекта, и виден только второй.
 *
 * Чего здесь нет и почему: содержимое файла, которое вернулось, должно
 * быть БАЙТ В БАЙТ состоянию снимка — но сравнивать его с тем, что мы
 * сами же и положили в снимок, значило бы проверять код кодом. Эталон
 * берётся из git (`git cat-file` по дереву) и с диска.
 * ====================================================================== */

namespace {

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

/* «Снимка нет» — ровно то, чем помечено состояние движка до первого
 * шага цикла. Отдельная функция, а не Snapshot{} в вызовах: пустой
 * снимок в тесте отката — это недосмотр, и без имени он выглядит как
 * ещё один аргумент. */
Snapshot no_snapshot() { return Snapshot(); }

} // namespace

TEST(revert_returns_the_project_to_the_state_of_the_snapshot) {
    SandBox sb("revert_git");
    const std::string repo = sb.project();
    write_file(fs::path(repo) / "a.txt", "one\n");
    write_file(fs::path(repo) / "b.txt", "b-original\n");
    init_repo_with_file(repo, "a.txt", "one\n");
    /* b.txt в первый коммит не вошёл: снимок берётся через stash, и файл
     * из индекса в дерево снимка не попадает. */
    raw_git(repo, "add -A");
    raw_git(repo, "commit -q -m second");

    const Snapshot snap = take(repo, sb.store());
    ASSERT_TRUE(snap.kind == Kind::GitTree);
    ASSERT_EQ(blob_in_tree(repo, snap.hash, "a.txt"), std::string("one\n"));

    /* Агент после снимка: изменил файл, удалил другой, создал новый. */
    write_file(fs::path(repo) / "a.txt", "two\n");
    raw_git(repo, "add a.txt");
    /* И правка поверх staged: индекс и рабочее дерево теперь разные, и
     * откат обязан вернуть рабочее дерево, не переписав индекс. */
    write_file(fs::path(repo) / "a.txt", "три\n");
    fs::remove(fs::path(repo) / "b.txt");
    write_file(fs::path(repo) / "new.txt", "создано агентом\n");

    /* Индекс неприкосновенен: человек подготовил a.txt к коммиту. */
    const std::string index_before = raw_git(repo, "diff --cached --name-only");
    ASSERT_TRUE(index_before.find("a.txt") != std::string::npos);

    const RestoreResult r = restore(snap.hash, snap, repo, sb.store());
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.source, std::string("git"));
    /* Содержимое — байт в байт состоянию снимка, а эталон взят из git. */
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("one\n"));
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"),
              blob_in_tree(repo, snap.hash, "a.txt"));
    /* Удалённый файл ВОЗВРАЩАЕТСЯ: он есть в снимке, и «восстановить
     * состояние» без этого означало бы «вернуть половину». */
    ASSERT_TRUE(fs::exists(fs::path(repo) / "b.txt"));
    ASSERT_EQ(read_text(fs::path(repo) / "b.txt"), std::string("b-original\n"));
    /* Файла, которого в снимке нет, откат НЕ удаляет: снимок не берёт
     * неотслеживаемые файлы, поэтому он его и не видел — «удалить всё,
     * чего в дереве нет» снесло бы и то, что снимок никогда не видел
     * (шапка core/snapshot.h, ограничение 1). */
    ASSERT_TRUE(fs::exists(fs::path(repo) / "new.txt"));
    ASSERT_EQ(read_text(fs::path(repo) / "new.txt"),
              std::string("создано агентом\n"));

    /* Список вернувшихся назван файлами, а не числом: по нему видно,
     * ЧТО именно восстановлено, и именно он потом попадёт в PatchPart
     * следующего хода. */
    ASSERT_EQ(r.restored.size(), (size_t)2);
    ASSERT_TRUE(contains(r.restored, "a.txt"));
    ASSERT_TRUE(contains(r.restored, "b.txt"));
    ASSERT_TRUE(r.leftover.empty());

    /* Индекс не переписан: снимок — это рабочее дерево, и его возврат
     * не должен переписывать то, что человек подготовил к коммиту.
     * Проверка содержательная: staged a.txt остался staged (пустой
     * список означал бы, что индекс перезаписали). */
    ASSERT_EQ(raw_git(repo, "diff --cached --name-only"), index_before);

    /* Второй откат того же снимка — не ошибка и не «что-то вернулось»:
     * состояние уже там. Иначе модель получила бы отказ там, где всё
     * в порядке, и начала бы искать обход. */
    const RestoreResult again = restore(snap.hash, snap, repo, sb.store());
    ASSERT_TRUE(again.ok);
    ASSERT_TRUE(again.restored.empty());
    ASSERT_TRUE(again.leftover.empty());
}

TEST(revert_does_not_reach_outside_the_project_directory) {
    /* Проект плагина — возможно подкаталог репозитория (монорепозиторий,
     * каталог данных рядом). Откат без pathspec вернул бы весь
     * репозиторий, то есть тронул бы чужой файл, который агент не
     * трогал и который не часть его работы. На проекте, который сам
     * является корнем репозитория, отличить одно от другого нельзя —
     * поэтому каталог данных проверки отдельный. */
    SandBox sb("revert_sub");
    const fs::path repo = fs::path(sb.root) / "repo";
    const fs::path app = repo / "wp-content";
    std::error_code ec;
    fs::create_directories(app / "deep", ec);
    write_file(repo / "снаружи.txt", "снаружи-исходное\n");
    write_file(app / "deep" / "z.php", "<?php\n");
    init_repo(repo.string());
    raw_git(repo.string(), "add -A");
    raw_git(repo.string(), "commit -q -m base");

    const Snapshot snap = take(app.string(), sb.store());
    ASSERT_TRUE(snap.kind == Kind::GitTree);

    write_file(app / "deep" / "z.php", "<?php // правка\n");
    write_file(repo / "снаружи.txt", "снаружи-ИЗМЕНЕНО\n");

    const RestoreResult r = restore(snap.hash, snap, app.string(), sb.store());
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(read_text(app / "deep" / "z.php"), std::string("<?php\n"));
    /* Чужой файл остался как был — иначе откат писал бы мимо проекта. */
    ASSERT_EQ(read_text(repo / "снаружи.txt"), std::string("снаружи-ИЗМЕНЕНО\n"));
    ASSERT_EQ(r.restored.size(), (size_t)1);
    ASSERT_EQ(r.restored.at(0), std::string("deep/z.php"));
}

TEST(revert_reports_what_it_could_not_return_instead_of_claiming_success) {
    /* Каталог, в который нельзя записать: git вернёт не ноль, и откат
     * обязан сказать об этом, а не отчитаться «2 файла вернулись».
     * Проверка ловит ровно тот подмен, который выглядит как успех:
     * список ДО отката, объявленный результатом. */
    SandBox sb("revert_fail");
    const std::string repo = sb.project();
    write_file(fs::path(repo) / "a.txt", "one\n");
    fs::create_directories(fs::path(repo) / "sub");
    write_file(fs::path(repo) / "sub" / "b.txt", "b\n");
    init_repo_with_file(repo, "a.txt", "one\n");
    raw_git(repo, "add -A");
    raw_git(repo, "commit -q -m second");

    const Snapshot snap = take(repo, sb.store());
    ASSERT_TRUE(snap.kind == Kind::GitTree);
    write_file(fs::path(repo) / "a.txt", "two\n");
    fs::remove(fs::path(repo) / "sub" / "b.txt");

    /* Права на каталог снимаем после снимка: без этого git создал бы
     * файл (в каталоге写) и откат прошёл бы вхолостую. */
    fs::permissions(fs::path(repo) / "sub", fs::perms::owner_read | fs::perms::owner_exec);
    const RestoreResult r = restore(snap.hash, snap, repo, sb.store());
    fs::permissions(fs::path(repo) / "sub",
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec);

    ASSERT_FALSE(r.ok);
    ASSERT_TRUE(r.reason.find("git") != std::string::npos);
    ASSERT_TRUE(r.restored.empty());
    ASSERT_TRUE(r.leftover.empty());
}

TEST(a_mode_only_difference_is_returned_by_the_rollback_too) {
    /* Права — тоже состояние, и откат возвращает их: проверено на живом
     * git 2.39, что `git restore --worktree` после `chmod +x` снова
     * делает файл неисполняемым, если в дереве бит не выставлен.
     *
     * Написать противоположное — «восстанавливается только содержимое» —
     * было бы враньём в шапке core/snapshot.h, и враньё это осело бы там
     * навсегда: проверить его можно было только на живом git, то есть не
     * в общем обзоре, а именно здесь. Первая версия проверки утверждала
     * ровно это и упала; вывод внесён в шапку и в журнал. */
    SandBox sb("revert_mode");
    const std::string repo = sb.project();
    write_file(fs::path(repo) / "a.txt", "one\n");
    init_repo_with_file(repo, "a.txt", "one\n");
    const Snapshot snap = take(repo, sb.store());
    ASSERT_TRUE(snap.kind == Kind::GitTree);

    /* Права меняются, содержимое — нет. */
    std::error_code ec;
    fs::permissions(fs::path(repo) / "a.txt",
                    fs::perms::owner_read | fs::perms::owner_write |
                    fs::perms::owner_exec);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("one\n"));

    const RestoreResult r = restore(snap.hash, snap, repo, sb.store());
    ASSERT_TRUE(r.ok);
    /* Файл отличался от снимка (по правам) и после отката не отличается —
     * то есть он вернулся, и попал в restored именно потому, что это
     * ПРОВЕРЕНО сравнением после, а не объявлено списком «до». */
    ASSERT_EQ(r.restored.size(), (size_t)1);
    ASSERT_EQ(r.restored.at(0), std::string("a.txt"));
    ASSERT_TRUE(r.leftover.empty());
    ASSERT_TRUE((fs::status(fs::path(repo) / "a.txt").permissions() &
                 fs::perms::owner_exec) == fs::perms::none);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("one\n"));
    /* И сам снимок теперь совпадает с проектом: повторный откат не должен
     * ни вернуть, ни пожаловаться. */
    const RestoreResult again = restore(snap.hash, snap, repo, sb.store());
    ASSERT_TRUE(again.ok);
    ASSERT_TRUE(again.restored.empty());
    ASSERT_TRUE(again.leftover.empty());
}

TEST(revert_refuses_a_hash_it_cannot_use_and_says_which_kind_it_is) {
    SandBox sb("revert_hash");
    const std::string repo = sb.project();
    write_file(fs::path(repo) / "a.txt", "one\n");
    init_repo_with_file(repo, "a.txt", "one\n");

    /* Не строка и не путь: раньше такие значения уходили в команду как
     * есть, и отказ выглядел бы как «снимок не найден», а причина была
     * бы в другом. */
    const RestoreResult junk =
        restore("snap-../../etc", no_snapshot(), repo, sb.store());
    ASSERT_FALSE(junk.ok);
    ASSERT_TRUE(junk.reason.find("не похож") != std::string::npos);
    /* Текст отказа назван целиком: модель должна понять, что прислала. */
    ASSERT_TRUE(junk.reason.find("snap-../../etc") != std::string::npos);

    /* Правильная форма, чужой объект: git не знает такого дерева, и
     * отказ приходит с его текстом — молчаливого «ничего не вернулось»
     * быть не должно. */
    const std::string absent(40, 'a');
    const RestoreResult missing = restore(absent, no_snapshot(), repo, sb.store());
    ASSERT_FALSE(missing.ok);
    ASSERT_TRUE(missing.reason.find("git не вернул") != std::string::npos ||
                missing.reason.find("не сравнил") != std::string::npos);

    /* Дерево git против проекта без git: развидеть нечего, и сказать
     * об этом лучше, чем искать копию с таким именем. */
    SandBox plain("revent_nogit");
    write_file(fs::path(plain.project()) / "a.txt", "one\n");
    const RestoreResult not_git = restore(absent, no_snapshot(),
                                          plain.project(), plain.store());
    ASSERT_FALSE(not_git.ok);
    ASSERT_TRUE(not_git.reason.find("не под git") != std::string::npos);
}

TEST(revert_of_a_copy_snapshot_returns_a_project_without_git) {
    /* Фолбэк «копия каталога» — не запасной путь, а единственный для
     * проекта без git, то есть для самого частого случая у человека,
     * который начал проект (журнал И10.1, п. 2б). Откат, работающий
     * только под git, был бы откатом на половине пользователей. */
    SandBox sb("revert_copy");
    const std::string repo = sb.project();
    write_file(fs::path(repo) / "a.txt", "one\n");
    /* Каталог создаётся явно: write_file этого файла не делает, и файл в
     * несуществующем каталоге молча не записался бы — снимок вышел бы
     * без него, а проверка «вернули удалённый файл» падала бы не по
     * существу. */
    std::error_code mk;
    fs::create_directories(fs::path(repo) / "sub", mk);
    write_file(fs::path(repo) / "sub" / "b.txt", "b\n");
    ASSERT_FALSE(is_git_repo(repo));

    const Snapshot snap = take(repo, sb.store());
    ASSERT_TRUE(snap.kind == Kind::DirCopy);
    ASSERT_TRUE(fs::is_directory(fs::path(snap.dir)));

    write_file(fs::path(repo) / "a.txt", "two\n");
    fs::remove(fs::path(repo) / "sub" / "b.txt");
    write_file(fs::path(repo) / "new.txt", "новое\n");

    const RestoreResult r = restore(snap.hash, snap, repo, sb.store());
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.source, std::string("копия каталога"));
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("one\n"));
    ASSERT_TRUE(fs::exists(fs::path(repo) / "sub" / "b.txt"));
    ASSERT_EQ(read_text(fs::path(repo) / "sub" / "b.txt"), std::string("b\n"));
    /* Отсутствие файла в копии — «нечего возвращать», а не «удалить». */
    ASSERT_TRUE(fs::exists(fs::path(repo) / "new.txt"));
    ASSERT_EQ(r.restored.size(), (size_t)2);
    ASSERT_TRUE(contains(r.restored, "a.txt"));
    ASSERT_TRUE(contains(r.restored, "sub/b.txt"));
    ASSERT_TRUE(r.leftover.empty());

    /* Повторный откат — тоже не ошибка: состояние уже там. */
    const RestoreResult again = restore(snap.hash, snap, repo, sb.store());
    ASSERT_TRUE(again.ok);
    ASSERT_TRUE(again.restored.empty());
}

TEST(revert_of_a_copy_says_which_files_it_could_not_return) {
    /* Файл, в который нельзя записать. Проверка ловит подмен «список ДО
     * отката назван результатом»: откат отчитался бы о вернутом файле,
     * содержимое которого осталось прежним, и модель продолжила бы
     * считать проект приведённым к снимку. */
    SandBox sb("revert_copy_left");
    const std::string repo = sb.project();
    write_file(fs::path(repo) / "a.txt", "one\n");
    const Snapshot snap = take(repo, sb.store());
    ASSERT_TRUE(snap.kind == Kind::DirCopy);
    write_file(fs::path(repo) / "a.txt", "two\n");

    fs::permissions(fs::path(repo) / "a.txt", fs::perms::owner_read);
    const RestoreResult r = restore(snap.hash, snap, repo, sb.store());
    fs::permissions(fs::path(repo) / "a.txt",
                    fs::perms::owner_read | fs::perms::owner_write);

    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.restored.empty());
    ASSERT_EQ(r.leftover.size(), (size_t)1);
    ASSERT_EQ(r.leftover.at(0), std::string("a.txt"));
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("two\n"));
}

TEST(revert_refuses_a_copy_taken_from_another_project) {
    /* Каталог копий общий для всех проектов плагина. Без сверки
     * «чьим проектом снята копия» откат по чужому хешу залил бы текущий
     * проект файлами того — молча и целиком, а имена у этих файлов
     * совпадают, потому что проекты похожи (шапка core/snapshot.h,
     * правило 3). */
    SandBox a("revert_owner_a");
    SandBox b("revert_owner_b");
    write_file(fs::path(a.project()) / "config.php", "<?php // A\n");
    write_file(fs::path(b.project()) / "config.php", "<?php // B\n");
    write_file(fs::path(b.project()) / "b.txt", "b-only\n");

    const Snapshot snap = take(a.project(), a.store());
    ASSERT_TRUE(snap.kind == Kind::DirCopy);

    const RestoreResult r = restore(snap.hash, snap, b.project(), a.store());
    ASSERT_FALSE(r.ok);
    ASSERT_TRUE(r.reason.find("другого проекта") != std::string::npos);
    ASSERT_TRUE(r.reason.find(a.project()) != std::string::npos);
    /* Отказ не тронул ни одного файла: проверка на содержимом, а не на
     * тексте причины. */
    ASSERT_EQ(read_text(fs::path(b.project()) / "config.php"),
              std::string("<?php // B\n"));
    ASSERT_EQ(read_text(fs::path(b.project()) / "b.txt"), std::string("b-only\n"));
}

TEST(a_copy_without_the_project_mark_cannot_be_restored) {
    /* Отметку снимают с копии — и такой снимок выглядит рабочим: каталог
     * на месте, файлы внутри есть. Откат обязан отказаться с названной
     * причиной, а не восстановить по подозрению: неизвестно, чей это
     * проект. */
    SandBox sb("revert_nomark");
    const std::string repo = sb.project();
    write_file(fs::path(repo) / "a.txt", "one\n");
    const Snapshot snap = take(repo, sb.store());
    ASSERT_TRUE(snap.kind == Kind::DirCopy);
    std::error_code ec;
    fs::remove(std::string(snap.dir) + kOwnerSuffix, ec);
    ASSERT_TRUE(ec ? false : !fs::exists(std::string(snap.dir) + kOwnerSuffix));

    write_file(fs::path(repo) / "a.txt", "two\n");
    const RestoreResult r = restore(snap.hash, snap, repo, sb.store());
    ASSERT_FALSE(r.ok);
    ASSERT_TRUE(r.reason.find("без отметки проекта") != std::string::npos);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("two\n"));
}

TEST(the_snapshot_of_the_current_step_is_used_when_the_hash_is_empty) {
    /* Пустой hash — снимок начала текущего шага (тот, что лежит в
     * состоянии движка). Так откатывает сам агент, посреди хода: хеш
     * снимка видит пользователь, а модели он не достаётся, и отправлять
     * служебную часть в транскрипт нельзя (отклонение 111).
     *
     * Проверка обязана звать restore() ИМЕННО с пустым hash: прежняя
     * версия звала его с «пробелами вокруг хеша» (обрезка — другой
     * случай), и проверка с именем «при пустом hash» не проверяла пустой
     * hash. Поймала это мутация «пустой hash не берёт снимок начала
     * шага»: поймала, но не та проверка, а соседняя. */
    SandBox sb("revert_empty");
    const std::string repo = sb.project();
    write_file(fs::path(repo) / "a.txt", "one\n");
    init_repo_with_file(repo, "a.txt", "one\n");

    const Snapshot last = take(repo, sb.store());
    ASSERT_TRUE(last.kind == Kind::GitTree);
    write_file(fs::path(repo) / "a.txt", "two\n");

    const RestoreResult r = restore("", last, repo, sb.store());
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.source, std::string("git"));
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("one\n"));
    ASSERT_EQ(r.restored.size(), (size_t)1);
    ASSERT_EQ(r.restored.at(0), std::string("a.txt"));

    /* Хеш с пробелами и переводом строки — модель присылает так часто, и
     * отказ «нет такого снимка» был бы враньём: снимок есть. */
    write_file(fs::path(repo) / "a.txt", "три\n");
    const RestoreResult padded = restore("  " + last.hash + "\n", last, repo,
                                         sb.store());
    ASSERT_TRUE(padded.ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("one\n"));

    /* Снимка начала шага нет — отказ с названной причиной, а не пустой
     * успех. Иначе модель решила бы, что откат состоялся. */
    Snapshot broken;
    broken.reason = "конфликт в индексе";
    const RestoreResult none = restore("", broken, repo, sb.store());
    ASSERT_FALSE(none.ok);
    ASSERT_TRUE(none.reason.find("конфликт в индексе") != std::string::npos);
    ASSERT_TRUE(none.reason.find("hash") != std::string::npos);
}

/* --- Инструмент `revert` (а не только функция) --- */

namespace {

/* Движок с проектом и каталогом данных фикстуры.
 *
 * Лямбда захватывает ФИКСТУРУ ПО ССЫЛКЕ и по причине из шапки SandBox:
 * копия фикстуры удаляла бы каталог в конце полного выражения, то есть
 * до первого же вызова инструмента. */
struct ToolFixture {
    SandBox sb;
    HostCallbacks cb;

    explicit ToolFixture(const char* what) : sb(what) {
        cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&,
                         LlmReply&) { return false; };
        cb.llm_complete = [](const std::string&, const std::string&,
                             std::string&) { return false; };
        cb.llm_is_connected = []() { return false; };
        cb.path_data_dir = [this] { return sb.store(); };
        cb.chat_event = [](const std::string&) {};
        Engine::instance().init(cb);
        register_base_tools();
        EngineState& state = engine_state();
        {
            std::lock_guard<std::mutex> lk(state.mtx);
            state.project_dir = sb.project();
            state.last_snapshot = Snapshot();
            state.plan_mode = false;
            state.mode = 0;
        }
    }
    ~ToolFixture() {
        EngineState& state = engine_state();
        std::lock_guard<std::mutex> lk(state.mtx);
        state.project_dir.clear();
        state.last_snapshot = Snapshot();
    }
    ToolFixture(const ToolFixture&) = delete;
    ToolFixture& operator=(const ToolFixture&) = delete;

    ToolOutput run(const std::string& hash) {
        json::JsonValue a = json::JsonValue::object();
        if (!hash.empty()) a.set("hash", json::JsonValue(hash));
        return ToolsRegistry::instance().run_output("revert", a);
    }
    /* Снимок кладётся в состояние так же, как его кладёт цикл: откат без
     * пустого hash берёт именно его. */
    void publish(const Snapshot& s) {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().last_snapshot = s;
    }
};

} // namespace

TEST(the_revert_tool_rolls_the_project_back_to_the_snapshot_of_the_step) {
    ToolFixture fx("revert_tool");
    write_file(fs::path(fx.sb.project()) / "a.txt", "one\n");
    init_repo_with_file(fx.sb.project(), "a.txt", "one\n");
    const Snapshot snap = take(fx.sb.project(), fx.sb.store());
    ASSERT_TRUE(snap.ok());
    fx.publish(snap);

    write_file(fs::path(fx.sb.project()) / "a.txt", "two\n");
    const ToolOutput o = fx.run("");
    ASSERT_TRUE(o.output.find("[revert]") != std::string::npos);
    ASSERT_TRUE(o.output.find("возвращено") != std::string::npos);
    ASSERT_TRUE(o.output.find("a.txt") != std::string::npos);
    ASSERT_EQ(read_text(fs::path(fx.sb.project()) / "a.txt"),
              std::string("one\n"));

    /* Ответ обязан называть границу отката словами: модель, увидевшая
     * «1 файл вернулся», считает проект приведённым к снимку, и файл,
     * созданный ею после снимка, который откат НЕ удаляет, был бы для неё
     * сюрпризом на следующем же шаге. */
    ASSERT_TRUE(o.output.find("не удалены") != std::string::npos);

    /* Повторный вызов: состояние уже там, и это сказано словами, а не
     * отказом — иначе модель начала бы искать обход. */
    const ToolOutput again = fx.run("");
    ASSERT_TRUE(again.output.find("уже совпадает") != std::string::npos);
}

TEST(the_revert_tool_says_plainly_that_it_did_not_work) {
    ToolFixture fx("revert_tool_fail");
    write_file(fs::path(fx.sb.project()) / "a.txt", "one\n");
    /* Снимка в состоянии нет: пустой hash нечего принимать за снимок. */
    const ToolOutput none = fx.run("");
    ASSERT_TRUE(none.output.find("откат не состоялся") != std::string::npos);
    ASSERT_TRUE(none.output.find("снимка начала шага нет") != std::string::npos);

    const ToolOutput junk = fx.run("что-то не то");
    ASSERT_TRUE(junk.output.find("откат не состоялся") != std::string::npos);
    ASSERT_TRUE(junk.output.find("не похож") != std::string::npos);
}

TEST(the_revert_tool_shows_a_long_list_of_returned_files) {
    /* Список файлов длинный: показывается начало, а число остальных
     * НАЗЫВАЕТСЯ. Молчаливый хвост выглядел бы как «вернулось 40», а
     * вернулось 60 — и следующий шаг агента строился бы на неправде.
     *
     * Проверка заодно держит форму списка на файлах, которых больше, чем
     * полос file_lock (64). Первая версия отката брала по file_lock::Guard
     * на каждый изменённый файл, и на 60 путях две из них гарантированно
     * попадают в одну полосу — то есть висят на нерекурсивном мьютексе.
     * Этот тест поймал то самое (его и видели по сторожу, а не по
     * падению), и по нему же видно, что deadlock-риск не вернулся. */
    ToolFixture fx("revert_tool_long");
    const std::string repo = fx.sb.project();
    for (int i = 0; i < 60; ++i) {
        write_file(fs::path(repo) / ("f" + std::to_string(i) + ".txt"), "one\n");
    }
    init_repo_with_file(repo, "f0.txt", "one\n");
    raw_git(repo, "add -A");
    raw_git(repo, "commit -q -m all");
    const Snapshot snap = take(repo, fx.sb.store());
    ASSERT_TRUE(snap.ok());
    for (int i = 0; i < 60; ++i) {
        write_file(fs::path(repo) / ("f" + std::to_string(i) + ".txt"), "two\n");
    }
    fx.publish(snap);

    const ToolOutput o = fx.run(snap.hash);
    ASSERT_TRUE(o.output.find("возвращено файлов: 60") != std::string::npos);
    ASSERT_TRUE(o.output.find("и ещё") != std::string::npos);
    ASSERT_EQ(read_text(fs::path(repo) / "f59.txt"), std::string("one\n"));
}
