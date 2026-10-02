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
#include "../core/shell.h"
#include "../core/snapshot.h"

#include "test_framework.h"

#include <unistd.h>

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
