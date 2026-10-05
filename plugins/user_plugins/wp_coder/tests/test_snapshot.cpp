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
#include "../core/limits.h"
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

/* Мусор из стека уровней сессии (И10.4): забрать под локом, удалить без
 * него (правило 3 — под state_.mtx каталоги не удаляют). Живёт здесь, а не
 * в разделе И10.4, потому что пользуется и фикстура инструментов ниже. */
void drain_stack_trash(EngineState& state) {
    std::vector<Snapshot> trash;
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        trash = state.undo_stack.take_trash();
    }
    discard_copies(trash);
}

/* Размер стека и «можно ли отменять» — тоже под локом: стек живёт в
 * состоянии синглтона, и чтение его полем мимо лока было бы гонкой,
 * пусть и сегодняшняя проверка однопоточная. */
size_t stack_size(EngineState& state) {
    std::lock_guard<std::mutex> lk(state.mtx);
    return state.undo_stack.size();
}

bool stack_can_undo(EngineState& state) {
    std::lock_guard<std::mutex> lk(state.mtx);
    return state.undo_stack.can_undo();
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
     std::error_code ec;
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
    /* `discard` — контракт вызывающего (шапка StepSnapshot): снимок, не
     * ставший уровнем, удаляется ПОСЛЕ того, как вызывающий прочитал его
     * через changed_files. Проверка его не убирала, и копия оставалась
     * лежать — то есть проверка сама была причиной утечки, которую искала. */
    discard_copies(first.discard);
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
     std::error_code ec;
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
     std::error_code ec;
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
     std::error_code ec;
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
     std::error_code ec;
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
            state.undo_stack.clear();
            state.plan_mode = false;
            state.mode = 0;
        }
    }
    ~ToolFixture() {
        EngineState& state = engine_state();
        {
            std::lock_guard<std::mutex> lk(state.mtx);
            state.project_dir.clear();
            state.last_snapshot = Snapshot();
            /* И10.4: стек — состояние синглтона, и его нельзя оставлять
             * следующей проверке (см. правило 5 SESSION_START). */
            state.undo_stack.clear();
        }
        drain_stack_trash(state);
        HostCallbacks safe;
        safe.chat_event = [](const std::string&) {};
        Engine::instance().init(safe);
    }
    ToolFixture(const ToolFixture&) = delete;
    ToolFixture& operator=(const ToolFixture&) = delete;

    ToolOutput run(const std::string& hash) {
        json::JsonValue a = json::JsonValue::object();
        if (!hash.empty()) a.set("hash", json::JsonValue(hash));
        return ToolsRegistry::instance().run_output("revert", a);
    }
    /* Инструмент без аргументов (undo, redo): пустой объект — это весь
     * их контракт, и отдельный метод защищает проверку от опечатки в
     * имени инструмента, которая выглядела бы как «инструмент отказал». */
    ToolOutput run_plain(const std::string& name) {
        return ToolsRegistry::instance().run_output(
            name, json::JsonValue::object());
    }
    /* Снимок кладётся в состояние так же, как его кладёт цикл: откат без
     * пустого hash берёт именно его. */
    void publish(const Snapshot& s) {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().last_snapshot = s;
    }
    /* Шаг цикла для инструментов undo/redo: тот же порядок, что в
     * take_step_snapshot, но без записи в last_snapshot — проверке
     * переходов состояние последнего шага не нужно. */
    void step() {
        const Snapshot s = take(sb.project(), sb.store());
        ASSERT_TRUE(s.ok());
        {
            std::lock_guard<std::mutex> lk(engine_state().mtx);
            engine_state().undo_stack.push(s);
        }
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

/* ======================================================================
 * И10.4: стек уровней сессии — undo и redo
 *
 * Тут то же, что и выше: настоящий git, настоящая файловая система и
 * никакой модели. Стек — это арифметика над состояниями, и проверять её
 * прогоном агента с сетью было бы фактически никак.
 *
 * Порядок проверок — по цене решения. Первая: ОДНО нажатие отменяет ОДИН
 * шаг. Ошибка здесь выглядит как работающий откат: файлы вернулись, и
 * вернулись не туда — на два шага назад вместо одного, то есть модель
 * потом строит своё следующее действие на состоянии, которого не
 * существует.
 * ====================================================================== */

namespace {

/* Шаг цикла агента: снимок состояния становится уровнем. Тот же порядок,
 * что в take_step_snapshot, и нарочно НЕ вызов той функции: проверка
 * стека не должна зависеть от кода, который она проверяет. */
void step_level(UndoStack& stack, const std::string& project,
                const std::string& store) {
    const Snapshot s = take(project, store);
    ASSERT_TRUE(s.ok());
    stack.push(s);
}

void write_content(const std::string& project, const char* name,
                   const std::string& text) {
    write_file(fs::path(project) / name, text);
}

/* Обрезка вывода git: ответы приходят с переводом строки, и сравнение
 * строк целиком проверяло бы формат вывода, а не смысл. */
std::string trimmed(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/* Сколько копий каталога лежит в каталоге данных. Именно каталогов, а не
 * всего содержимого: рядом с каждой копией лежит её отметка `.owner`
 * (отклонение 123), и счётчик файлов считал бы их парами. */
size_t copy_dirs(const std::string& store) {
    std::error_code ec;
    size_t n = 0;
    for (fs::directory_iterator it(fs::path(store), ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec) && !ec) ++n;
    }
    return n;
}

/* Копии снимков, снятых ДВИЖКОМ (take_step_snapshot), лежат в
 * store_dir(data_dir), то есть на уровень глубже, чем копии, снятые
 * тестом напрямую через take(project, sb.store()). Считать каталоги в
 * data_dir и принимать это за число копий — версия проверки, которая
 * считает папку `wp_coder` и потому проходит при любом числе снимков. */
size_t copy_dirs_in_store(const std::string& data_dir) {
    return copy_dirs(store_dir(data_dir));
}

} // namespace

TEST(undo_returns_one_step_back_and_redo_returns_it_forward) {
    SandBox sb("undo_redo");
    const std::string repo = sb.project();
    write_content(repo, "a.txt", "one\n");
    init_repo_with_file(repo, "a.txt", "one\n");

    UndoStack stack;
    /* Шаг 1: состояние «one». */
    step_level(stack, repo, sb.store());
    /* Шаг 1 поработал. */
    write_content(repo, "a.txt", "two\n");
    /* Шаг 2: состояние «two». */
    step_level(stack, repo, sb.store());
    /* Шаг 2 поработал, и мы внутри него — модель зовёт undo посреди хода. */
    write_content(repo, "a.txt", "три\n");

    const MoveResult u = stack.undo(repo, sb.store());
    ASSERT_TRUE(u.ok);
    /* Ровно ОДИН шаг назад: «три» → «two», а не сразу в «one». */
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("two\n"));
    ASSERT_EQ(u.restored.size(), (size_t)1);
    ASSERT_EQ(u.restored.at(0), std::string("a.txt"));
    ASSERT_TRUE(u.can_undo);
    ASSERT_TRUE(u.can_redo);
    ASSERT_EQ(u.level, (size_t)2);
    ASSERT_EQ(u.levels, (size_t)3);

    const MoveResult r = stack.redo(repo, sb.store());
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("три\n"));
    ASSERT_FALSE(r.can_redo);
    /* Всё, что было до отмены, вернулось одним уровнем вперёд, а не
     * «а теперь вернулось ещё и то, что было дальше»: стек кончился. */
    ASSERT_FALSE(stack.redo(repo, sb.store()).ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("три\n"));
}

TEST(undo_walks_back_several_levels_and_redo_walks_forward_again) {
    /* Многоуровневость — это и есть задача: одно нажатие отменяет один
     * шаг, а не «всё до начала». Четыре уровня и по два перехода в
     * каждую сторону: середина стека проверяется обоими краями. */
    SandBox sb("undo_levels");
    const std::string repo = sb.project();
    write_content(repo, "a.txt", "one\n");
    init_repo_with_file(repo, "a.txt", "one\n");

    UndoStack stack;
    step_level(stack, repo, sb.store());
    for (int i = 2; i <= 4; ++i) {
        write_content(repo, "a.txt", "level" + std::to_string(i) + "\n");
        step_level(stack, repo, sb.store());
    }
    write_content(repo, "a.txt", "current\n");
    ASSERT_EQ(stack.size(), (size_t)4);

    ASSERT_TRUE(stack.undo(repo, sb.store()).ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("level4\n"));
    ASSERT_TRUE(stack.undo(repo, sb.store()).ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("level3\n"));
    ASSERT_TRUE(stack.undo(repo, sb.store()).ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("level2\n"));
    /* Дальше назад некуда: «one» — это уровень, но добраться до него
     * можно только ещё одним нажатием, и стек это позволяет. */
    ASSERT_TRUE(stack.undo(repo, sb.store()).ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("one\n"));

    /* А теперь обратно — до самого верха, то есть до того состояния,
     * которого на диске уже нет. */
    for (int i = 2; i <= 4; ++i) {
        ASSERT_TRUE(stack.redo(repo, sb.store()).ok);
        ASSERT_EQ(read_text(fs::path(repo) / "a.txt"),
                  "level" + std::to_string(i) + "\n");
    }
    ASSERT_TRUE(stack.redo(repo, sb.store()).ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("current\n"));
    ASSERT_FALSE(stack.redo(repo, sb.store()).ok);
    ASSERT_EQ(stack.size(), (size_t)5);
}

TEST(a_step_that_changed_nothing_does_not_become_a_level) {
    /* Уровень — СОСТОЯНИЕ, а не шаг. Если бы пустой шаг завёл уровень,
     * «отменить» пришлось бы жать вхолостую: модель отменила бы пустоту
     * и решила, что откат не работает. */
    SandBox sb("undo_noop");
    const std::string repo = sb.project();
    write_content(repo, "a.txt", "one\n");
    init_repo_with_file(repo, "a.txt", "one\n");

    UndoStack stack;
    step_level(stack, repo, sb.store());
    write_content(repo, "a.txt", "two\n");
    step_level(stack, repo, sb.store());
    /* Шаг, который ничего не сделал. */
    step_level(stack, repo, sb.store());
    ASSERT_EQ(stack.size(), (size_t)2);

    write_content(repo, "a.txt", "три\n");
    ASSERT_TRUE(stack.undo(repo, sb.store()).ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("two\n"));
    ASSERT_TRUE(stack.undo(repo, sb.store()).ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("one\n"));
}

TEST(undo_says_it_has_nothing_to_undo_and_changes_nothing) {
    /* Отказ с названной причиной, а не пустой успех: модель, получившая
     * «отменено» при отказе, продолжила бы считать проект откатанным. */
    SandBox sb("undo_nothing");
    const std::string repo = sb.project();
    write_content(repo, "a.txt", "one\n");
    init_repo_with_file(repo, "a.txt", "one\n");

    UndoStack empty;
    const MoveResult none = empty.undo(repo, sb.store());
    ASSERT_FALSE(none.ok);
    ASSERT_TRUE(none.reason.find("стек уровней пуст") != std::string::npos);
    ASSERT_EQ(none.level, (size_t)0);
    ASSERT_EQ(none.levels, (size_t)0);

    UndoStack stack;
    step_level(stack, repo, sb.store());
    /* Мы стоим на самом раннем уровне и ничего с него не меняли. */
    const MoveResult first = stack.undo(repo, sb.store());
    ASSERT_FALSE(first.ok);
    ASSERT_TRUE(first.reason.find("самое раннее состояние") != std::string::npos);
    ASSERT_TRUE(first.can_undo);   /* уровень есть, отменять нечего */
    ASSERT_FALSE(first.can_redo);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("one\n"));
}

TEST(redo_is_refused_while_the_agent_has_edited_after_the_undo) {
    /* Тот же запрет (правило 2 шапки), но в окне, которого ещё не
     * закрыл push(): отмена, потом правка посреди того же хода, и
     * возврат вперёд — уровни впереди в стеке ещё лежат, они просто
     * больше не впереди. Отказ с названной причиной, а не прыжок через
     * сделанное. */
    SandBox sb("undo_redo_edited");
    const std::string repo = sb.project();
    write_content(repo, "a.txt", "one\n");
    init_repo_with_file(repo, "a.txt", "one\n");

    UndoStack stack;
    step_level(stack, repo, sb.store());
    write_content(repo, "a.txt", "two\n");
    step_level(stack, repo, sb.store());
    write_content(repo, "a.txt", "три\n");
    ASSERT_TRUE(stack.undo(repo, sb.store()).ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("two\n"));

    /* Агент поработал после отмены — следующего шага ещё не было. */
    write_content(repo, "a.txt", "снова работа\n");
    const MoveResult r = stack.redo(repo, sb.store());
    ASSERT_FALSE(r.ok);
    ASSERT_TRUE(r.reason.find("после новых правок") != std::string::npos);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"),
              std::string("снова работа\n"));
}

TEST(new_work_after_an_undo_kills_the_way_forward) {
    /* Возврат вперёд не перепрыгивает через сделанное: правило 2 шапки
     * core/snapshot.h. Проверка содержит и вторую половину — отказ с
     * причиной, а не «уровень пропал»: по коду ответа модель обязана
     * понять, что вернуться вперёд больше некуда. */
    SandBox sb("undo_redo_dead");
    const std::string repo = sb.project();
    write_content(repo, "a.txt", "one\n");
    init_repo_with_file(repo, "a.txt", "one\n");

    UndoStack stack;
    step_level(stack, repo, sb.store());
    write_content(repo, "a.txt", "two\n");
    step_level(stack, repo, sb.store());
    write_content(repo, "a.txt", "три\n");
    ASSERT_TRUE(stack.undo(repo, sb.store()).ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("two\n"));
    ASSERT_TRUE(stack.can_redo());

    /* Новый шаг после отмены: работа агента пошла дальше. */
    write_content(repo, "a.txt", "four\n");
    step_level(stack, repo, sb.store());
    ASSERT_FALSE(stack.can_redo());
    /* Уровней стало три, а не два: отменённое состояние осталось
     * уровнем (оно и было целью возврата), а вперёд ушло только то, что
     * отменять уже не собирались. */
    ASSERT_EQ(stack.size(), (size_t)3);

    const MoveResult r = stack.redo(repo, sb.store());
    ASSERT_FALSE(r.ok);
    ASSERT_TRUE(r.reason.find("нечего возвращать вперёд") != std::string::npos);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("four\n"));
}

TEST(new_work_after_an_undo_is_itself_undoable) {
    /* Продолжение предыдущей проверки: новый шаг после ДВУХ отмен не только
     * убивает возврат вперёд, но и сам обязан отменяться.
     *
     * Именно две отмены, а не одна: состояние, оставленное переходом,
     * непустое только там, куда переход уже приходил.
     *
     * Здесь ломается РАСХОЖДЕНИЕ ИНДЕКСОВ. Стек помнит состояние, которое
     * оставил переход (шапка UndoStack, отклонение 122), и держит его
     * параллельно уровням. Новый шаг подрезает хвост уровней вперёд — и
     * хвост состояний обязан уйти вместе с ним: если он останется, то
     * после подрезки и push() индексы разъедутся, состояние, оставленное
     * ОТМЕНЁННЫМ уровнем, встанет на место нового уровня, и следующая
     * отмена решит, что проект упирается вперёд, — то есть отменит уже
     * отменённый шаг вместо нового. Проверка ловит это содержимым файла,
     * а не падением.
     *
     * Второй эффект того же расхождения — копии: оставшиеся состояния
     * никто не удалит (отклонение 109), а на не-git-проекте это каталоги. */
    SandBox sb("undo_after_new_work");
    std::error_code ec;
    const std::string repo = sb.project();
    write_content(repo, "a.txt", "v0\n");

    UndoStack stack;
    stack.push(take(repo, sb.store()));            /* уровень 0 */
    write_content(repo, "a.txt", "v1\n");
    stack.push(take(repo, sb.store()));            /* уровень 1 */
    write_content(repo, "a.txt", "v2\n");
    write_content(repo, "side.txt", "создано вторым шагом\n");
    stack.push(take(repo, sb.store()));            /* уровень 2 */

    ASSERT_TRUE(stack.undo(repo, sb.store()).ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("v1\n"));
    ASSERT_TRUE(fs::exists(fs::path(repo) / "side.txt"));
    /* Вторая отмена: без неё расхождение индексов не видно — состояние,
     * оставленное на уровне 2, при подрезке хвоста ещё пустое и на место
     * нового уровня не встаёт. */
    ASSERT_TRUE(stack.undo(repo, sb.store()).ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("v0\n"));

    /* Новая работа: уровни 1 и 2 и их состояния уходят в хвост. */
    write_content(repo, "a.txt", "v3\n");
    stack.push(take(repo, sb.store()));
    ASSERT_EQ(stack.size(), (size_t)2);
    ASSERT_FALSE(stack.can_redo());

    /* Отмена нового шага обязана вернуть ИМЕННО его. */
    ASSERT_TRUE(stack.undo(repo, sb.store()).ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("v0\n"));
    (void)ec;
}

TEST(undo_returns_a_deleted_file_and_keeps_a_created_one) {
    /* Граница 1 шапки core/snapshot.h: отмена — не обращение функции.
     std::error_code ec;
     * Удалённый файл вернётся, созданный — останется. Обе половины
     * проверяются здесь, потому что проверить одну и не заметить
     * вторую легко, а читателю ответа инструмента это стоило бы
     * сюрприза на следующем же шаге. */
    SandBox sb("undo_created");
    const std::string repo = sb.project();
    write_content(repo, "a.txt", "one\n");
    write_content(repo, "b.txt", "b\n");
    init_repo_with_file(repo, "a.txt", "one\n");
    write_content(repo, "b.txt", "b\n");
    raw_git(repo, "add -A");
    raw_git(repo, "commit -q -m two");

    UndoStack stack;
    step_level(stack, repo, sb.store());
    write_content(repo, "a.txt", "two\n");
    std::error_code ec;
    fs::remove(fs::path(repo) / "b.txt", ec);
    write_content(repo, "new.txt", "создано агентом\n");

    ASSERT_TRUE(stack.undo(repo, sb.store()).ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("one\n"));
    ASSERT_TRUE(fs::exists(fs::path(repo) / "b.txt"));
    ASSERT_EQ(read_text(fs::path(repo) / "b.txt"), std::string("b\n"));
    ASSERT_TRUE(fs::exists(fs::path(repo) / "new.txt"));
    ASSERT_EQ(read_text(fs::path(repo) / "new.txt"),
              std::string("создано агентом\n"));
}

TEST(undo_refuses_a_level_it_cannot_restore_and_keeps_the_stack_usable) {
    /* Отказ не должен съедать стек: иначе одна неудачная отмена отняла
     std::error_code ec;
     * бы и возможность вернуться вперёд. Проверяется на копии каталога,
     * потому что уровень git нельзя испортить руками — его объект живёт
     * в базе репозитория.
     *
     * Проверка составная намеренно: «отказался» мало. Сломанный уровень
     * лежит ПОД целевым, поэтому проверяется и то, что выбитый уровень
     * не вычеркнут из стека молча, и то, что ход назад после отказа
     * по-прежнему работает. */
    SandBox sb("undo_broken");
    const std::string repo = sb.project();
    write_content(repo, "a.txt", "one\n");

    UndoStack stack;
    step_level(stack, repo, sb.store());
    write_content(repo, "a.txt", "two\n");
    step_level(stack, repo, sb.store());
    write_content(repo, "a.txt", "three\n");
    const Snapshot third = take(repo, sb.store());
    ASSERT_TRUE(third.kind == Kind::DirCopy);
    stack.push(third);
    /* Снимок начала шага, чьи правки отменяются прямо сейчас. */
    write_content(repo, "a.txt", "four\n");

    ASSERT_TRUE(stack.undo(repo, sb.store()).ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("three\n"));
    const size_t before = stack.size();
    const size_t at = stack.position();

    /* Уровень, к которому пойдёт следующая отмена, снесли с диска. */
    const Snapshot second = stack.level(1);
    std::error_code ec;
    fs::remove_all(fs::path(second.dir), ec);

    const MoveResult u = stack.undo(repo, sb.store());
    ASSERT_FALSE(u.ok);
    ASSERT_TRUE(u.reason.find("не найдена") != std::string::npos);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("three\n"));
    /* Стек цел и стоит на месте: ни уровня не вычеркнуто, ни позиция не
     * съехала. */
    ASSERT_EQ(stack.size(), before);
    ASSERT_EQ(stack.position(), at);
    ASSERT_TRUE(stack.can_redo());

    /* Мусор, собранный на отказе, удаляем — как это делает вызывающий, —
     * и копия уровня, на котором мы стоим, обязана уцелеть. Снятое на
     * отказе состояние это ТОТ ЖЕ уровень, и удаление по списку без
     * сверки снесло бы копию, которая ещё нужна стеку. */
    const std::vector<Snapshot> trash = stack.take_trash();
    discard_copies(trash);
    ASSERT_TRUE(fs::exists(fs::path(stack.level(at).dir)));

    /* Возврат вперёд после отказа работает — и это единственное, что
     * может доказать, что отказ не испортил стек: содержимое уровня на
     * месте, отказ его не тронул. */
    const MoveResult r = stack.redo(repo, sb.store());
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("four\n"));
}

TEST(the_stack_is_dropped_when_the_project_changes) {
    /* Уровни чужого проекта вернули бы сюда его файлы — молча и целиком,
     std::error_code ec;
     * потому что у похожих проектов имена файлов совпадают (то же, чем
     * отличается приём отклонения 123). Сверка идёт по полю снимка,
     * потому что проект меняется прямой записью в состояние из UI и
     * крючка на смену нет. */
    SandBox sb("undo_project");
    const std::string other = (fs::path(sb.root) / "other").string();
    std::error_code ec;
    fs::create_directories(other, ec);
    write_content(sb.project(), "a.txt", "one\n");

    UndoStack stack;
    const Snapshot a = take(sb.project(), sb.store());
    ASSERT_TRUE(a.ok());
    stack.push(a);
    write_content(sb.project(), "a.txt", "two\n");
    step_level(stack, sb.project(), sb.store());
    ASSERT_EQ(stack.size(), (size_t)2);

    /* Снимок чужого проекта приходит на первом же его шаге. */
    write_content(other, "z.php", "<?php\n");
    stack.push(take(other, sb.store()));
    ASSERT_EQ(stack.size(), (size_t)1);
    ASSERT_EQ(stack.level(0).project_dir, other);
    /* Копия вытесненного уровня удаляется ВЫЗЫВАЮЩИМ (стек только
     * отдаёт): вне стека её никто не прочтёт, а на не-git-проекте это
     * целый каталог (отклонение 109). */
    const std::vector<Snapshot> trash = stack.take_trash();
    ASSERT_TRUE(!trash.empty());
    discard_copies(trash);
    ASSERT_FALSE(fs::exists(fs::path(a.dir)));
    ASSERT_FALSE(fs::exists(fs::path(a.dir + std::string(kOwnerSuffix))));

    /* Отмена в чужом стеке для прежнего проекта — отказ, а не откат. */
    const MoveResult r = stack.undo(sb.project(), sb.store());
    ASSERT_FALSE(r.ok);
    ASSERT_TRUE(r.reason.find("другого проекта") != std::string::npos);
}

TEST(a_level_that_fell_out_of_the_stack_is_deleted_from_the_store) {
    /* Предел глубины и уборка копий — решение задачи (отклонение 109):
     * без них каталог данных плагина рос бы на полную копию проекта
     * каждый шаг всей сессии. Git-уровни при этом НЕ трогаются: их
     * объект лежит в базе репозитория, и обещать его удаление было бы
     * враньём — вместо этого проверяется, что он и не исчезает. */
    SandBox sb("undo_depth");
    const std::string repo = sb.project();
    write_content(repo, "a.txt", "one\n");

    UndoStack stack;
    const Snapshot oldest = take(repo, sb.store());
    ASSERT_TRUE(oldest.kind == Kind::DirCopy);
    stack.push(oldest);
    const size_t depth = limits::kSnapshotStackDepth;
    for (size_t i = 1; i < depth + 1; ++i) {
        write_content(repo, "a.txt", "step" + std::to_string(i) + "\n");
        step_level(stack, repo, sb.store());
    }
    ASSERT_EQ(stack.size(), depth);
    /* Мусор снят стеком, но удаляет его вызывающий — поэтому проверка
     * берёт то, что вернул take_trash(). */
    const std::vector<Snapshot> trash = stack.take_trash();
    ASSERT_TRUE(!trash.empty());
    discard_copies(trash);
    ASSERT_FALSE(fs::exists(fs::path(oldest.dir)));
    /* Отменить вытесненное уже нельзя, и это сказано честно: уровней
     * ровно предел, а не «сколько влезло», и самый ранний из них —
     * «step1», а не «one». */
    while (stack.undo(repo, sb.store()).ok) {
    }
    ASSERT_EQ(stack.size(), depth);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("step1\n"));
}

TEST(a_git_level_fallen_out_of_the_stack_is_not_deleted) {
    /* Вторая половина той же границы: вытесненный уровень под git
     * остаётся пригодным для revert по хешу — объект в базе репозитория
     * удалить нечем, да и незачем. Проверка на живой команде git, а не на
     * том, вернул ли discard_copies() ошибку. */
    SandBox sb("undo_git_evicted");
    const std::string repo = sb.project();
    write_content(repo, "a.txt", "one\n");
    init_repo_with_file(repo, "a.txt", "one\n");

    const Snapshot oldest = take(repo, sb.store());
    ASSERT_TRUE(oldest.kind == Kind::GitTree);
    discard_copies(std::vector<Snapshot>{oldest});
    ASSERT_EQ(trimmed(raw_git(repo, "cat-file -t " + oldest.hash)),
              std::string("tree"));
    /* И откат по нему по-прежнему работает. */
    write_content(repo, "a.txt", "two\n");
    const RestoreResult r = restore(oldest.hash, oldest, repo, sb.store());
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("one\n"));
}

TEST(a_step_snapshot_pushes_a_level_into_the_engine_state) {
    /* Шов с циклом: уровень заводит ТОТ ЖЕ вызов, что и last_snapshot.
     * Два вызова означали бы два одинаковых уровня, и «отменить» жало
     * бы вхолостую на каждом шаге.
     *
     * Первая половина — проект без git и без каталога данных: снимок не
     * состоялся, а значит не появился и уровень с правдоподобным
     * содержимым. Такой уровень отменял бы несуществующее состояние. */
    EngineState& state = engine_state();
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        state.project_dir.clear();
        state.undo_stack.clear();
    }
    drain_stack_trash(state);
    HostCallbacks empty_cb;
    empty_cb.path_data_dir = [] { return ""; };
    const StepSnapshot none = take_step_snapshot(state, empty_cb, nullptr);
    ASSERT_FALSE(none.after.ok());
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        ASSERT_EQ(state.undo_stack.size(), (size_t)0);
        ASSERT_TRUE(state.undo_stack.can_undo() == false);
    }

    /* Вторая половина — настоящий проект: два вызова подряд дают ОДИН
     * уровень, потому что состояние между ними не менялось. */
    SandBox sb("undo_push");
    write_content(sb.project(), "a.txt", "one\n");
    init_repo_with_file(sb.project(), "a.txt", "one\n");
    HostCallbacks cb;
    cb.path_data_dir = [&sb] { return sb.store(); };
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        state.project_dir = sb.project();
    }
    const StepSnapshot s1 = take_step_snapshot(state, cb, nullptr);
    const StepSnapshot s2 = take_step_snapshot(state, cb, nullptr);
    ASSERT_TRUE(s1.after.ok());
    ASSERT_TRUE(s2.after.ok());
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        ASSERT_EQ(state.undo_stack.size(), (size_t)1);
        /* Снимок начала шага и уровень — одно и то же состояние. */
        ASSERT_EQ(state.undo_stack.level(0).hash, s2.before.hash);
    }
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        state.project_dir.clear();
        state.last_snapshot = Snapshot();
        state.undo_stack.clear();
    }
    drain_stack_trash(state);
}

TEST(undo_does_not_move_the_snapshot_of_the_current_step) {
    /* Граница 4 шапки core/snapshot.h: пустой hash у revert по-прежнему
     * значит «состояние на начало ТЕКУЩЕГО шага». Если бы undo двигал
     * last_snapshot, отмена молча превратилась бы в «уже совпадает», и
     * модель решила бы, что отменённое вернулось назад, а оно лежит
     * дальше по стеку.
     *
     * Проверка уводит отмену ЗА начало шага (две отмены), иначе откат к
     * снимку начала шага совпал бы с тем, куда отмена уже пришла, и
     * разницы команд не было бы видно. */
    SandBox sb("undo_keeps_last");
    const std::string repo = sb.project();
    write_content(repo, "a.txt", "one\n");
    init_repo_with_file(repo, "a.txt", "one\n");

    UndoStack stack;
    step_level(stack, repo, sb.store());
    write_content(repo, "a.txt", "two\n");
    step_level(stack, repo, sb.store());
    write_content(repo, "a.txt", "три\n");
    step_level(stack, repo, sb.store());
    /* Снимок начала ТЕКУЩЕГО шага — «три», а отмены уводят ниже. */
    const Snapshot step_start = take(repo, sb.store());
    write_content(repo, "a.txt", "four\n");

    ASSERT_TRUE(stack.undo(repo, sb.store()).ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("три\n"));
    ASSERT_TRUE(stack.undo(repo, sb.store()).ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("two\n"));

    /* Откат к снимку начала шага возвращает проект ВПЕРЁД — и это сказано
     * словами в шапке, а не здесь. */
    const RestoreResult r = restore("", step_start, repo, sb.store());
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("три\n"));
}

/* --- Инструменты `undo` и `redo` (а не только стек) --- */

TEST(the_undo_and_redo_tools_walk_the_stack_and_say_what_is_possible) {
    /* Проверяется ИНСТРУМЕНТ, а не функция: у него есть ответ, который
     * читает модель, и этот ответ обязан называть границы. Молчаливый
     * «отменено» при отказе заставил бы модель считать проект откатанным
     * и строить следующий шаг на неправде. */
    ToolFixture fx("undo_tool");
    const std::string repo = fx.sb.project();
    write_content(repo, "a.txt", "one\n");
    init_repo_with_file(repo, "a.txt", "one\n");
    fx.step();
    write_content(repo, "a.txt", "two\n");
    fx.step();
    write_content(repo, "a.txt", "три\n");

    const ToolOutput u = fx.run_plain("undo");
    ASSERT_TRUE(u.output.find("[undo]") != std::string::npos);
    ASSERT_TRUE(u.output.find("возвращено файлов: 1") != std::string::npos);
    ASSERT_TRUE(u.output.find("a.txt") != std::string::npos);
    ASSERT_TRUE(u.output.find("уровень 2 из 3") != std::string::npos);
    /* Что можно дальше — часть ответа, а не деталь реализации: без неё
     * модель жмёт «redo» наугад. */
    ASSERT_TRUE(u.output.find("вернуть вперёд: да") != std::string::npos);
    ASSERT_TRUE(u.output.find("не удалены") != std::string::npos);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("two\n"));

    const ToolOutput r = fx.run_plain("redo");
    ASSERT_TRUE(r.output.find("[redo]") != std::string::npos);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("три\n"));
    ASSERT_TRUE(r.output.find("вернуть вперёд: нет") != std::string::npos);
    ASSERT_TRUE(r.output.find("отменить ещё: да") != std::string::npos);

    /* Возврат вперёд состоялся, значит отменять есть куда — дважды, до
     * самого раннего уровня. Инструмент обязан различать «есть куда» и
     * «уже некуда»: первое — работа, второе — отказ с причиной. */
    const ToolOutput back = fx.run_plain("undo");
    ASSERT_TRUE(back.output.find("[undo]") != std::string::npos);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("two\n"));
    ASSERT_TRUE(back.output.find("вернуть вперёд: да") != std::string::npos);
    const ToolOutput back2 = fx.run_plain("undo");
    ASSERT_TRUE(back2.output.find("уровень 1 из 3") != std::string::npos);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("one\n"));
    /* can_undo() отвечает «есть ли уровень», а не «вернёт ли вызов»:
     * уровень есть, отменять нечего, и обе правды нужны в ответе. */
    ASSERT_TRUE(back2.output.find("отменить ещё: да") != std::string::npos);
    ASSERT_TRUE(back2.output.find("вернуть вперёд: да") != std::string::npos);

    const ToolOutput end = fx.run_plain("undo");
    ASSERT_TRUE(end.output.find("отменять нечего или не вышло") != std::string::npos);
    ASSERT_TRUE(end.output.find("самое раннее состояние") != std::string::npos);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("one\n"));
}

TEST(the_undo_and_redo_tools_say_so_when_the_stack_is_empty) {
    ToolFixture fx("undo_tool_empty");
    const ToolOutput u = fx.run_plain("undo");
    ASSERT_TRUE(u.output.find("отменять нечего или не вышло") != std::string::npos);
    ASSERT_TRUE(u.output.find("стек уровней пуст") != std::string::npos);
    ASSERT_TRUE(u.output.find("уровней нет") != std::string::npos);
    /* «уровней нет», а не «уровень 1 из 0» — номер без количества
     * читался бы как позиция. */
    ASSERT_TRUE(u.output.find("уровень 1 из") == std::string::npos);

    const ToolOutput r = fx.run_plain("redo");
    ASSERT_TRUE(r.output.find("возвращать нечего или не вышло") != std::string::npos);
    ASSERT_TRUE(r.output.find("нечего возвращать вперёд") != std::string::npos);
}

TEST(the_undo_tools_are_hidden_from_the_looking_agent_by_the_revert_key) {
    /* Один ключ на три инструмента — не только про удобство вопроса, но
     * и про правила готовых агентов: агент-поиск запрещает ключ revert
     * целиком, и undo/redo скрылись у него сами. Отдельная проверка
     * стоит того: молчаливый переезд на другой ключ оставил бы
     * агенту-поиску отмену, и счётчик скрытых инструментов это увидел
     * бы не сразу. */
    register_base_tools();
    const auto defs = ToolsRegistry::instance().defs();
    for (const char* name : {"revert", "undo", "redo"}) {
        const ToolDef* def = ToolsRegistry::instance().find(name);
        ASSERT_TRUE(def != nullptr);
        if (!def) continue;      /* find() вернул nullptr — иначе проверка
                                   * ниже читала бы несуществующий объект */
        ASSERT_EQ(def->permission_key, std::string("revert"));
        ASSERT_TRUE(tf_has(def->flags, TF_DESTRUCTIVE));
        ASSERT_TRUE(tf_has(def->flags, TF_WRITES_FILES));
    }
    ASSERT_TRUE(!defs.empty());
}

TEST(a_cleared_session_forgets_the_levels_and_drops_their_copies) {
    /* «Очистить сессию» — это кнопка «начать с чистого листа». Держать
     * после неё уровни, к которым можно откатиться, значило бы оставить
     * одну из тех вещей, которые человек только что попросил забыть.
     *
     * Копия проверяется на диске, а не в векторе: стек мог бы обнулиться
     * и оставить каталоги, то есть уборка оказалась бы декларацией. */
    SandBox sb("undo_clear_session");
    write_content(sb.project(), "a.txt", "one\n");
    const Snapshot s = take(sb.project(), sb.store());
    ASSERT_TRUE(s.kind == Kind::DirCopy);
    EngineState& state = engine_state();
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        state.undo_stack.clear();
    }
    drain_stack_trash(state);
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        state.undo_stack.push(s);
    }
    ASSERT_EQ(stack_size(state), (size_t)1);
    ASSERT_TRUE(stack_can_undo(state));

    engine().clear_session();

    ASSERT_EQ(stack_size(state), (size_t)0);
    ASSERT_FALSE(stack_can_undo(state));
    ASSERT_FALSE(fs::exists(fs::path(s.dir)));
    ASSERT_FALSE(fs::exists(fs::path(s.dir + std::string(kOwnerSuffix))));
}

TEST(undo_at_the_oldest_level_survives_the_depth_limit) {
    /* Предел глубины не должен съедать уровень, НА КОТОРОМ СТОИТ проект.
     * Сценарий: стек полон, агент откатился до самого старого уровня,
     * поправил файл и снова зовёт undo — вытеснение старого уровня увело
     * бы позицию под ноль, и следующий переход читал бы уровень мимо
     * массива. Найдено разбором падения мутированного кода, а проверка
     * написана после, чтобы случай не остался непокрытым: без мутации он
     * просто молча ломал бы стек в длинной сессии.
     *
     * Проект под git — снимок дешёвый (tree-hash), и проверка укладывается
     * в сторож: 51 уровень и столько же отмен — это команды git, а не
     * копирование каталога. */
    SandBox sb("undo_depth_bottom");
    const std::string repo = sb.project();
    write_content(repo, "a.txt", "one\n");
    init_repo_with_file(repo, "a.txt", "one\n");

    UndoStack stack;
    const size_t depth = limits::kSnapshotStackDepth;
    for (size_t i = 0; i < depth; ++i) {
        write_content(repo, "a.txt", "step" + std::to_string(i) + "\n");
        step_level(stack, repo, sb.store());
    }
    /* До самого старого уровня. */
    while (stack.undo(repo, sb.store()).ok) {
    }
    ASSERT_EQ(stack.position(), (size_t)0);
    ASSERT_TRUE(stack.can_undo());

    /* Правка после отката до дна, затем ещё одна отмена. */
    write_content(repo, "a.txt", "правка после дна\n");
    const MoveResult u = stack.undo(repo, sb.store());
    ASSERT_TRUE(u.ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"),
              "step" + std::to_string(0) + "\n");
    /* Позиция осталась в границах стека, и уровень, на котором мы стоим,
     * в стеке есть: иначе следующий переход читал бы мимо массива. */
    ASSERT_TRUE(stack.position() < stack.size());
    ASSERT_TRUE(!stack.level(stack.position()).hash.empty());
    /* Стек мог превысить предел на один уровень — стоять на вытесненном
     * нельзя, а новый уровень появиться может. Назад отмена работает. */
    ASSERT_TRUE(stack.size() <= depth + 1);
    ASSERT_TRUE(stack.undo(repo, sb.store()).ok == false);
}

TEST(a_refused_move_leaves_no_copy_of_the_state_it_took) {
    /* Отказ — тоже снятие состояния: переход берёт копию проекта ДО того,
     * как узнает, что переходить некуда. На не-git-проекте это целый
     * каталог, и если такую копию не отдать в мусор, она остаётся на диске
     * навсегда — а отказ при этом самый частый случай: «отменить» на
     * самом раннем уровне человек жмёт машинально, и каждый такой press
     * оставлял бы после себя каталог.
     *
     * Проверка ловит не «отказ состоялся», а УТЕЧКУ, и потому смотрит на
     * каталог копий, а не на ответ. Случай был найден мутацией: снятие
     * guard-а `!holds(here)` («уровень с таким состоянием уже есть, значит
     * копию не выбрасывать») ВЫЖИЛО — то есть ни одна проверка этого не
     * различала.
     *
     * Совпадение состояния не делает копию чужой: у снимка DirCopy каталог
     * свой (идентификатор выдаёт take(), отклонение 123 — отметка `.owner`
     * лежит рядом), и на отказе снимок в стек не попадает, поэтому
     * ссылаться на его копию некому. */
    SandBox sb("undo_refused_copy");
    const std::string repo = sb.project();
    write_content(repo, "a.txt", "one\n");

    UndoStack stack;
    step_level(stack, repo, sb.store());
    const size_t before = copy_dirs(sb.store());

    /* Стоим на самом раннем уровне, состояние то же: отказ с причиной. */
    const MoveResult u = stack.undo(repo, sb.store());
    ASSERT_FALSE(u.ok);
    ASSERT_TRUE(u.reason.find("самое раннее состояние") != std::string::npos);

    discard_copies(stack.take_trash());
    ASSERT_EQ(copy_dirs(sb.store()), before);
    /* Копия уровня, на котором мы стоим, осталась на месте: отказ не
     * имеет права убирать то, чем стек пользуется. */
    ASSERT_TRUE(stack.can_undo());
    ASSERT_TRUE(!stack.level(stack.position()).dir.empty());
    ASSERT_TRUE(fs::exists(fs::path(stack.level(stack.position()).dir)));
}

TEST(a_refused_redo_leaves_no_copy_either) {
    /* Тот же случай на втором отказе — «после новых правок возвращаться
     * вперёд нельзя». Отказов у undo три (пустой стек, чужой проект, самое
     * раннее состояние) и у redo один, и проверять их всех в одной
     * нельзя было бы дёшево: mutation-прогон снял бы guard во всех трёх
     * отказах undo сразу и поймал бы утечку в ОДНОМ из них, а остальные два
     * остались бы непокрытыми навсегда. Здесь — самый частый из отказов
     * undo плюс отказ redo. */
    SandBox sb("undo_refused_redo");
    const std::string repo = sb.project();
    write_content(repo, "a.txt", "one\n");

    UndoStack stack;
    step_level(stack, repo, sb.store());
    write_content(repo, "a.txt", "two\n");
    step_level(stack, repo, sb.store());
    write_content(repo, "a.txt", "три\n");
    ASSERT_TRUE(stack.undo(repo, sb.store()).ok);
    discard_copies(stack.take_trash());
    const size_t before = copy_dirs(sb.store());

    /* Агент поработал после отмены: возвращаться вперёд нельзя, но
     * состояние к отказу всё равно снято. */
    write_content(repo, "a.txt", "новая работа\n");
    const MoveResult r = stack.redo(repo, sb.store());
    ASSERT_FALSE(r.ok);
    ASSERT_TRUE(r.reason.find("после новых правок") != std::string::npos);

    discard_copies(stack.take_trash());
    ASSERT_EQ(copy_dirs(sb.store()), before);
    /* Стек остался пригодным: отказ — не поломка, и вернуться назад
     * по-прежнему можно. */
    ASSERT_TRUE(stack.can_undo());
}

/* ======================================================================
 * И10.5: diff для каждого снапшота доступен UI
 *
 * Проверяется ровно то, что обещает задача: по ИМЕНОВАННОМУ состоянию
 * получается список файлов с патчами, и этот список — то же самое, что
 * человек видит в `git diff`. Эталон git сверяется посимвольно, иначе
 * «мы печатаем unified» было бы утверждением без проверки.
 *
 * Порядок — по цене решения. Первое: имена файлов и их счётчики, потому
 * что ошибка здесь выглядит как рабочий откат («отмена вернула не то»).
 * Второе: отказы, потому что «отчёт пуст» и «отчёт не удалось» — разные
 * факты, и человек по первому решил бы, что правок не было.
 * ====================================================================== */

namespace {

/* Тело unified-патка из вывода git: всё от `--- `, без преамбулы
 * (`diff --git`, `index`) и без хвоста `@@ … @@ <function>`, который
 * git дописывает по своей эвристике. Наш генератор печатает ровно это
 * тело — иначе один и тот же файл выглядел бы по-разному в зависимости
 * от того, git-уровень это или копия каталога. */
std::string git_body(std::string out) {
    const size_t start = out.find("--- ");
    if (start == std::string::npos) return "";
    out = out.substr(start);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    size_t pos = 0;
    while ((pos = out.find("@@ -", pos)) != std::string::npos) {
        const size_t close = out.find("@@", pos + 3);
        if (close == std::string::npos) break;
        const size_t line_end = out.find('\n', close);
        const size_t tail_end = line_end == std::string::npos ? out.size() : line_end;
        out.erase(close + 2, tail_end - (close + 2));
        pos = close + 2;
    }
    return out;
}

/* Найти файл в отчёте. */
const diff::FileDiff* find_file(const DiffReport& r, const std::string& name) {
    for (const diff::FileDiff& f : r.files) {
        if (f.file == name) return &f;
    }
    return nullptr;
}

} // namespace

TEST(diff_states_names_every_changed_file_with_gits_own_patch) {
    /* ГЛАВНОЕ ПРОВЕРЕНИЕ ЗАДАЧИ: список файлов и патчи совпадают с тем,
     * что печатает git на тех же двух состояниях. Сверка посимвольная —
     * иначе проверка ловила бы «мы что-то напечатали», а не «мы
     * напечатали то же, что git». */
    SandBox sb("diff_states");
    const std::string repo = sb.project();
    write_content(repo, "one.txt", "a\nb\nc\n");
    write_content(repo, "two.txt", "x\ny\n");
    init_repo_with_file(repo, "one.txt", "a\nb\nc\n");
    raw_git(repo, "add -A");
    raw_git(repo, "commit -q -m base");

    const Snapshot before = take(repo, sb.store());
    ASSERT_TRUE(before.kind == Kind::GitTree);

    write_content(repo, "one.txt", "a\nB\nc\nd\n");
    write_content(repo, "two.txt", "x\n");
    const Snapshot after = take(repo, sb.store());
    ASSERT_TRUE(after.kind == Kind::GitTree);

    const DiffReport r = diff_states(before, after, sb.store());
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.from_hash, before.hash);
    ASSERT_EQ(r.to_hash, after.hash);
    ASSERT_EQ(r.files.size(), (size_t)2);
    ASSERT_FALSE(r.truncated);
    ASSERT_EQ(r.omitted, (size_t)0);
    /* Счётчики — по обоим файлам сразу: one.txt это +2/−1, two.txt это
     * +0/−1, то есть +2 и −2. Ожидание «одно удаление» относилось к
     * одному файлу и проверяло бы арифметику не того объекта. */
    ASSERT_EQ(r.additions(), (size_t)2);
    ASSERT_EQ(r.deletions(), (size_t)2);

    const diff::FileDiff* one = find_file(r, "one.txt");
    const diff::FileDiff* two = find_file(r, "two.txt");
    ASSERT_TRUE(one != nullptr);
    ASSERT_TRUE(two != nullptr);
    if (!one || !two) return;
    ASSERT_EQ(one->additions, (size_t)2);
    ASSERT_EQ(one->deletions, (size_t)1);
    ASSERT_EQ(two->additions, (size_t)0);
    ASSERT_EQ(two->deletions, (size_t)1);
    ASSERT_TRUE(one->patch.find("+B\n") != std::string::npos);
    /* two.txt теряет ПОСЛЕДНЮЮ строку, поэтому `-y` — последняя строка
     * патча и завершающего перевода у неё нет. Проверяется и сама
     * форма: патч хранится без завершающего перевода — так его печатает
     * наш генератор и так же приводится вывод git, иначе у git-уровней
     * в окне была бы лишняя пустая строка, а у копий каталога нет. */
    ASSERT_TRUE(two->patch.find("-y") != std::string::npos);
    ASSERT_TRUE(!two->patch.empty());
    ASSERT_TRUE(two->patch.back() != '\n');

    /* Посимвольная сверка с живым git на тех же деревьях.
     *
     * Эталон берётся ОТДЕЛЬНО по одному пути: общий вывод git содержит
     * блок на каждый файл, и сверка патча одного файла с ним целиком
     * сравнивала бы разные вещи — первая версия проверки именно так и
     * делала, и падала на чужом блоке. */
    const std::string git_patch = git_body(raw_git(
        repo, "diff --no-color --no-renames -U3 --relative " + before.hash + " " +
                  after.hash + " -- one.txt"));
    ASSERT_EQ(one->patch, git_patch);
    ASSERT_TRUE(!git_patch.empty());
}

TEST(diff_states_reports_paths_relative_to_the_project_not_the_repo) {
    /* Проект — ПОДКАРАЛОГ репозитория, и без `--relative` git печатал бы
     std::error_code ec;
     * пути от корня репозитория («wp/deep/f.txt» вместо «f.txt»). Человек
     * увидел бы в окне файл, которого в его проекте нет, и diff перестал
     * бы быть «про его проект». */
    SandBox sb("diff_relative");
    std::error_code ec;
    fs::create_directories(fs::path(sb.root) / "outer", ec);
    const std::string outer = (fs::path(sb.root) / "outer").string();
    fs::create_directories(fs::path(outer) / "deep", ec);
    init_repo(outer);
    write_file(fs::path(outer) / "deep" / "f.txt", "a\nb\n");
    raw_git(outer, "add -A");
    raw_git(outer, "commit -q -m base");

    const std::string project = (fs::path(outer) / "deep").string();
    const Snapshot before = take(project, sb.store());
    write_file(fs::path(project) / "f.txt", "a\nB\n");
    const Snapshot after = take(project, sb.store());
    const DiffReport r = diff_states(before, after, sb.store());
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.files.size(), (size_t)1);
    ASSERT_EQ(r.files.at(0).file, std::string("f.txt"));
}

TEST(diff_states_names_an_added_and_a_deleted_file_on_the_git_path) {
/* Добавленный и удалённый файл — это ДВА разных вида изменений, и
     * для каждого git печатает свою пару заголовков (`--- /dev/null`
     * либо `+++ /dev/null`). Если разбор берёт имя только из `---`,
     * добавленный файл остался бы без имени, а удалённый — получил бы
     * имя из `+++ /dev/null`, то есть «/dev/null» вместо своего. */
    SandBox sb("diff_add_del");
    std::error_code ec;
    const std::string repo = sb.project();
    write_content(repo, "stay.txt", "a\n");
    write_content(repo, "gone.txt", "b\n");
    init_repo_with_file(repo, "stay.txt", "a\n");
    raw_git(repo, "add -A");
    raw_git(repo, "commit -q -m base");

    const Snapshot before = take(repo, sb.store());
    fs::remove(fs::path(repo) / "gone.txt", ec);
    write_content(repo, "new.txt", "c\n");
    /* `git add` здесь — обязательная часть сценария, а не украшение:
     * неотслеживаемый файл не попадает в дерево снимка (отклонение 107),
     * и первая версия проверки ждала его в отчёте, то есть требовала
     * нарушить границу, о которой сама же и говорит. */
    raw_git(repo, "add -A");
    const Snapshot after = take(repo, sb.store());

    const DiffReport r = diff_states(before, after, sb.store());
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.files.size(), (size_t)2);
    const diff::FileDiff* added = find_file(r, "new.txt");
    const diff::FileDiff* deleted = find_file(r, "gone.txt");
    ASSERT_TRUE(added != nullptr);
    ASSERT_TRUE(deleted != nullptr);
    if (!added || !deleted) return;
    ASSERT_EQ(added->additions, (size_t)1);
    ASSERT_EQ(deleted->deletions, (size_t)1);
    ASSERT_TRUE(added->patch.find("--- /dev/null") != std::string::npos);
    ASSERT_TRUE(deleted->patch.find("+++ /dev/null") != std::string::npos);
}

TEST(a_binary_change_is_named_and_not_counted_as_line_edits) {
    /* Двоичный файл в отчёте ЕСТЬ, но без строк: у git для него одна
     * строка вместо hunk'а. Если бы она попала в патч, получился бы
     * файл с нулём строк и чужой строкой внутри; если бы он молча
     * пропал — человек увидел бы «изменений нет» при изменённом файле. */
    SandBox sb("diff_binary");
    const std::string repo = sb.project();
    std::ofstream bin(fs::path(repo) / "bin.dat", std::ios::binary);
    bin << std::string("a\0b", 3) << "\n";
    bin.close();
    write_content(repo, "t.txt", "a\n");
    init_repo(repo);
    raw_git(repo, "add -A");
    raw_git(repo, "commit -q -m base");

    const Snapshot before = take(repo, sb.store());
    std::ofstream bin2(fs::path(repo) / "bin.dat", std::ios::binary);
    bin2 << std::string("a\0c", 3) << "\n";
    bin2.close();
    const Snapshot after = take(repo, sb.store());

    const DiffReport r = diff_states(before, after, sb.store());
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.files.size(), (size_t)1);
    const diff::FileDiff* b = find_file(r, "bin.dat");
    ASSERT_TRUE(b != nullptr);
    if (!b) return;
    ASSERT_TRUE(b->binary);
    ASSERT_EQ(b->additions, (size_t)0);
    ASSERT_EQ(b->deletions, (size_t)0);
    ASSERT_TRUE(b->patch.empty());
    ASSERT_TRUE(!b->note.empty());
}

TEST(diff_states_works_on_a_directory_copy_too) {
    /* Тот же вопрос на не-git-проекте: там состояние — это каталог, и
     std::error_code ec;
     * diff строится нашим кодом, а не берётся у git. Проверка держит
     * и содержимое патча, и то, что отчёт НЕ пуст: пустой отчёт при
     * изменённом файле — это «правок нет», и человек нажал бы отмену
     * не зная, что вернётся. */
    SandBox sb("diff_copy");
    std::error_code ec;
    const std::string repo = sb.project();
    write_content(repo, "a.txt", "one\ntwo\n");
    write_content(repo, "b.txt", "keep\n");
    /* Без init_repo: проект не под git, и take() делает копию каталога. */
    const Snapshot before = take(repo, sb.store());
    ASSERT_TRUE(before.kind == Kind::DirCopy);

    write_content(repo, "a.txt", "one\nTWO\n");
    fs::remove(fs::path(repo) / "b.txt", ec);
    write_content(repo, "c.txt", "new\n");
    const Snapshot after = take(repo, sb.store());

    const DiffReport r = diff_states(before, after, sb.store());
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.files.size(), (size_t)3);
    const diff::FileDiff* a = find_file(r, "a.txt");
    ASSERT_TRUE(a != nullptr);
    if (!a) return;
    ASSERT_EQ(a->additions, (size_t)1);
    ASSERT_EQ(a->deletions, (size_t)1);
    ASSERT_TRUE(a->patch.find("-two") != std::string::npos);
    ASSERT_TRUE(a->patch.find("+TWO") != std::string::npos);
    ASSERT_TRUE(find_file(r, "b.txt") != nullptr);
    ASSERT_TRUE(find_file(r, "c.txt") != nullptr);
}

TEST(diff_states_refuses_what_it_cannot_compare_and_says_why) {
    /* Два отказа, и оба обязаны быть НАЗВАНЫ: пустой отчёт читался бы
     std::error_code ec;
     * как «правок нет», и человек счёл бы, что отменять нечего. */
    DiffReport r = diff_states(Snapshot(), Snapshot(), "");
    ASSERT_FALSE(r.ok);
    ASSERT_TRUE(r.reason.find("не состоялся") != std::string::npos);

    /* Снимки разных видов: git-путь работает с двумя деревьями, копия —
     * с двумя каталогами. Молча взять «что-то одно» значило бы показать
     * diff не того. */
    SandBox sb("diff_mixed");
    std::error_code ec;
    write_content(sb.project(), "a.txt", "x\n");
    init_repo_with_file(sb.project(), "a.txt", "x\n");
    const Snapshot git_level = take(sb.project(), sb.store());
    /* remove_all, а не remove: fs::remove на каталоге молча ничего не
     * делает (только для пустого), и проверка получала GitTree вместо
     * DirCopy — то есть проверяла не тот случай, который собиралась. */
    fs::remove_all(fs::path(sb.project()) / ".git", ec);
    const Snapshot copy_level = take(sb.project(), sb.store());
    ASSERT_TRUE(git_level.kind == Kind::GitTree);
    ASSERT_TRUE(copy_level.kind == Kind::DirCopy);

    r = diff_states(git_level, copy_level, sb.store());
    ASSERT_FALSE(r.ok);
    ASSERT_TRUE(r.reason.find("разных видов") != std::string::npos);
}

TEST(diff_step_shows_exactly_what_the_undo_of_that_level_would_change) {
    /* Вопрос человека, нажимающего «отменить», — «что изменится», а не
     * «что менялось за сессию». Поэтому уровень сравнивается со СЛЕДУЮЩИМ
     * уровнем, а не с первым и не с последним. Ошибка здесь выглядит как
     * работающая отмена с неверным предпросмотром: человек читает diff и
     * жмёт «отменить» в расчёте на одно, а получает другое. */
    SandBox sb("diff_step");
    const std::string repo = sb.project();
    write_content(repo, "a.txt", "one\n");
    init_repo_with_file(repo, "a.txt", "one\n");

    UndoStack stack;
    step_level(stack, repo, sb.store());          /* уровень 0: one */
    write_content(repo, "a.txt", "two\n");
    step_level(stack, repo, sb.store());          /* уровень 1: two */
    write_content(repo, "a.txt", "three\n");
    step_level(stack, repo, sb.store());          /* уровень 2: three */
    ASSERT_EQ(stack.position(), (size_t)2);

    /* Уровень 0 отвечает на вопрос «что сделал первый шаг»: one → two. */
    const DiffReport first = stack.diff_step(0, repo, sb.store());
    ASSERT_TRUE(first.ok);
    ASSERT_EQ(first.files.size(), (size_t)1);
    ASSERT_TRUE(first.files.at(0).patch.find("-one") != std::string::npos);
    ASSERT_TRUE(first.files.at(0).patch.find("+two") != std::string::npos);

    /* Уровень 1: two → three. */
    const DiffReport second = stack.diff_step(1, repo, sb.store());
    ASSERT_TRUE(second.ok);
    ASSERT_EQ(second.files.size(), (size_t)1);
    ASSERT_TRUE(second.files.at(0).patch.find("-two") != std::string::npos);
    ASSERT_TRUE(second.files.at(0).patch.find("+three") != std::string::npos);

    /* Верхний уровень сравнивается с тем, что на диске, а оно равно
     * ему самому, поэтому diff ПУСТ — и это правда, а не сбой: шага
     * после него не было, смотреть не на что. Первая версия проверки
     * ждала здесь непустой отчёт и требовала несуществующей правки:
     * отмена этого уровня вернула бы проект в то же состояние. */
    const DiffReport top = stack.diff_step(stack.position(), repo, sb.store());
    ASSERT_TRUE(top.ok);
    ASSERT_TRUE(top.files.empty());
}

TEST(diff_step_of_the_top_level_compares_it_with_the_working_directory) {
    /* У последнего уровня следующего нет — «куда» — это то, что лежит на
     * диске сейчас. Без этого верхний уровень нельзя было бы посмотреть
     * вовсе, а именно его человек и смотрит первым: он отвечает на вопрос
     * «что сделал последний шаг». */
    SandBox sb("diff_step_top");
    const std::string repo = sb.project();
    write_content(repo, "a.txt", "one\n");
    init_repo_with_file(repo, "a.txt", "one\n");

    UndoStack stack;
    step_level(stack, repo, sb.store());
    write_content(repo, "a.txt", "правка на диске\n");
    /* Файл, которого в уровне нет: отмена его НЕ удалит (отклонение 122),
     * и diff обязан это показать как добавление, а не промолчать. */
    write_content(repo, "fresh.txt", "новый\n");

    const DiffReport r = stack.diff_step(0, repo, sb.store());
    ASSERT_TRUE(r.ok);
    /* Один файл: fresh.txt НЕОТСЛЕЖИВАЕМЫЙ, и в дерево снимка он не
     * попадает (отклонение 107). Первая версия проверки ждала двух
     * файлов и удивлялась, что git-путь не видит созданный агентом
     * файл, то есть требовала нарушения документированной границы.
     * Здесь граница закреплена проверкой: человек обязан понимать, что
     * его новый файл отменой не вернётся. */
    ASSERT_EQ(r.files.size(), (size_t)1);
    ASSERT_TRUE(find_file(r, "fresh.txt") == nullptr);
    const diff::FileDiff* a = find_file(r, "a.txt");
    ASSERT_TRUE(a != nullptr);
    if (!a) return;
    ASSERT_TRUE(a->patch.find("+правка на диске") != std::string::npos);
}

TEST(diff_step_refuses_an_index_the_stack_does_not_have) {
    /* Отказ с NAMED причиной, а не пустой отчёт: пустой читался бы как
     * «правок нет». */
    SandBox sb("diff_step_bad");
    write_content(sb.project(), "a.txt", "one\n");

    UndoStack stack;
    const DiffReport empty_stack = stack.diff_step(0, sb.project(), sb.store());
    ASSERT_FALSE(empty_stack.ok);
    ASSERT_TRUE(empty_stack.reason.find("ступеней нет") != std::string::npos);

    step_level(stack, sb.project(), sb.store());
    const DiffReport past_end = stack.diff_step(7, sb.project(), sb.store());
    ASSERT_FALSE(past_end.ok);
    ASSERT_TRUE(past_end.reason.find("уровня 7 нет") != std::string::npos);
}

TEST(level_diff_reads_the_engine_stack_and_changes_nothing_in_it) {
    /* Шов к UI. Проверяются две вещи, и обе существенны:
     *   (1) отчёт получился — то есть метод действительно читает стек
     *       движка, а не пустой синглтон;
     *   (2) стек после НЕ изменился. Просмотр не имеет права двигать
     *       позицию: человек открыл посмотреть и нажал «отменить» на
     *       другом уровне — откат ушёл бы не туда. */
    EngineState& state = engine_state();
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        state.project_dir.clear();
        state.undo_stack.clear();
    }
    drain_stack_trash(state);

    SandBox sb("level_diff");
    const std::string repo = sb.project();
    write_content(repo, "a.txt", "one\n");
    init_repo_with_file(repo, "a.txt", "one\n");

    HostCallbacks cb;
    cb.path_data_dir = [&sb] { return sb.store(); };
    engine().init(cb);
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        state.project_dir = repo;
    }
    step_level(state.undo_stack, repo, sb.store());
    write_content(repo, "a.txt", "two\n");
    step_level(state.undo_stack, repo, sb.store());
    write_content(repo, "a.txt", "three\n");
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        ASSERT_EQ(state.undo_stack.size(), (size_t)2);
        ASSERT_EQ(state.undo_stack.position(), (size_t)1);
    }

    const DiffReport r = engine().level_diff(1);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.files.size(), (size_t)1);
    ASSERT_TRUE(r.files.at(0).patch.find("-two") != std::string::npos);
    ASSERT_TRUE(r.files.at(0).patch.find("+three") != std::string::npos);

    {
        std::lock_guard<std::mutex> lk(state.mtx);
        ASSERT_EQ(state.undo_stack.size(), (size_t)2);
        ASSERT_EQ(state.undo_stack.position(), (size_t)1);
        state.project_dir.clear();
        state.undo_stack.clear();
    }
    drain_stack_trash(state);
}

TEST(the_name_of_a_git_block_must_agree_with_numstat) {
    /* Сверка обязательна, а проверить её на живом репозитории НЕЛЬЗЯ: ни
     * один сценарий не заставит git напечатать блок не для того файла.
     * Поэтому разбор вынесен в отдельную функцию (отклонение 110 — там же
     * про parse_hash) и проверяется на ТЕКСТЕ.
     *
     * Мутация «имя не сверяется с numstat» без такой проверки ВЫЖИЛА, и
     * механизм назван: имя бралось бы из заголовка блока, а при
     * несовпадении показывался бы патч ЧУЖОГО файла — молча и целиком. */
    DiffReport r;
    const std::string numstat = std::string("1\t1\tfirst.txt", 14) + '\0';
    const std::string patch =
        "diff --git a/first.txt b/first.txt\n"
        "index 1111111..2222222 100644\n"
        "--- a/first.txt\n"
        "+++ b/first.txt\n"
        "@@ -1 +1 @@\n"
        "-a\n"
        "+b\n";
    ASSERT_TRUE(diff_from_git_text(numstat, patch, &r));
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.files.size(), (size_t)1);
    ASSERT_EQ(r.files.at(0).file, std::string("first.txt"));

    /* Тот же блок, но numstat называет ДРУГОЙ файл: показывать нельзя. */
    const std::string wrong = std::string("1\t1\tsecond.txt", 15) + '\0';
    ASSERT_FALSE(diff_from_git_text(wrong, patch, &r));
    ASSERT_FALSE(r.ok);
    ASSERT_TRUE(r.reason.find("first.txt") != std::string::npos);
    ASSERT_TRUE(r.reason.find("second.txt") != std::string::npos);
}

TEST(a_binary_git_block_is_paired_with_its_file_by_order) {
    /* У двоичного файла и у изменения только прав в unified-выводе НЕТ ни
     * `---`, ни `+++`: имя есть только в неоднозначном заголовке
     * `diff --git`, и берётся оно из numstat — то есть по ПОРЯДКУ.
     * Порядок этот проверяется на тексте с двумя блоками, где у первого
     * имя есть, а у второго нет: если бы порядок был не тот, отчёт показал
     * бы у первого файла чужое имя — и проверка это увидит. */
    DiffReport r;
    std::string numstat;
    numstat += std::string("1\t1\ta.txt", 9) + '\0';
    numstat += std::string("-\t-\tbin.dat", 12) + '\0';
    const std::string patch =
        "diff --git a/a.txt b/a.txt\n"
        "index 1111111..2222222 100644\n"
        "--- a/a.txt\n"
        "+++ b/a.txt\n"
        "@@ -1 +1 @@\n"
        "-x\n"
        "+y\n"
        "diff --git a/bin.dat b/bin.dat\n"
        "index 3333333..4444444 100644\n"
        "Binary files a/bin.dat and b/bin.dat differ\n";
    ASSERT_TRUE(diff_from_git_text(numstat, patch, &r));
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.files.size(), (size_t)2);
    ASSERT_EQ(r.files.at(0).file, std::string("a.txt"));
    ASSERT_EQ(r.files.at(0).additions, (size_t)1);
    ASSERT_EQ(r.files.at(1).file, std::string("bin.dat"));
    ASSERT_TRUE(r.files.at(1).binary);
    ASSERT_EQ(r.files.at(1).additions, (size_t)0);
    ASSERT_TRUE(r.files.at(1).patch.empty());
    ASSERT_TRUE(!r.files.at(1).note.empty());
}

TEST(a_mode_only_change_is_named_and_says_that_there_are_no_lines) {
    /* Изменились права, а не строки: git печатает `old mode`/`new mode`
     * и НИ ОДНОЙ строки. Сказать «правок нет» было бы неправдой —
     * изменение есть, просто не в строках, — поэтому файл называется и
     * объясняет. */
    DiffReport r;
    std::string numstat;
    numstat += std::string("0\t0\tscript.sh", 13) + '\0';
    const std::string patch =
        "diff --git a/script.sh b/script.sh\n"
        "old mode 100644\n"
        "new mode 100755\n";
    ASSERT_TRUE(diff_from_git_text(numstat, patch, &r));
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.files.size(), (size_t)1);
    ASSERT_EQ(r.files.at(0).file, std::string("script.sh"));
    ASSERT_EQ(r.files.at(0).additions, (size_t)0);
    ASSERT_TRUE(r.files.at(0).patch.empty());
    ASSERT_TRUE(r.files.at(0).note.find("права") != std::string::npos);
}

TEST(more_git_blocks_than_files_in_numstat_is_a_refusal) {
    /* На один файл больше блоков, чем имён: значит, показывать нечего
     * однозначно, и отчёт без предупреждения был бы списком с пропуском,
     * который выглядел бы как полный. */
    DiffReport r;
    const std::string numstat = std::string("1\t1\tonly.txt", 13) + '\0';
    const std::string patch =
        "diff --git a/one.txt b/one.txt\n"
        "--- a/one.txt\n"
        "+++ b/one.txt\n"
        "@@ -1 +1 @@\n"
        "-a\n"
        "+b\n"
        "diff --git a/two.txt b/two.txt\n"
        "--- a/two.txt\n"
        "+++ b/two.txt\n"
        "@@ -1 +1 @@\n"
        "-c\n"
        "+d\n";
    ASSERT_FALSE(diff_from_git_text(numstat, patch, &r));
    ASSERT_FALSE(r.ok);
    ASSERT_TRUE(!r.reason.empty());
}

/* ======================================================================
 * И10.6: тесты — не-git каталог, откат нескольких файлов, целостность
 *       при прерывании
 *
 * Задача 10.6 — coverage, а не код: три названных случая. Что уже было
 * покрыто до неё и НЕ дублируется здесь: снимок и восстановление на
 * не-git каталоге (`revert_of_a_copy_snapshot_returns_a_project_without_git`,
 * `revert_of_a_copy_says_which_files_it_could_not_return`,
 * `revert_refuses_a_copy_taken_from_another_project`), откат НЕСКОЛЬКИХ
 * файлов на git-пути (`the_revert_tool_shows_a_long_list_of_returned_files` —
 * 60 файлов), устойчивость стека при отказе
 * (`undo_refuses_a_level_it_cannot_restore_and_keeps_the_stack_usable`).
 * Ниже — то, чего не было: несколько файлов на пути копии каталога, и
 * то, что прерывание не оставляет снимковый строй в полусостоянии.
 * ====================================================================== */

TEST(undo_returns_several_files_at_once_from_a_copy_snapshot) {
    /* Несколько файлов на пути КОПИИ КАТАЛОГА. На git-пути это проверено
     * (60 файлов), а на копии — нет, а путь другой: файлы сравниваются
     * побайтно и возвращаются копированием, а не командами git.
     *
     * Три вида изменения сразу, потому что на каждом свой чужой путь:
     * изменённый, созданный и удалённый. Проверяется и список вернувшихся
     * — он идёт в ответ инструмента и в UI, то есть это не деталь
     * реализации, а то, что человек читает. */
    SandBox sb("undo_copy_many");
    std::error_code ec;
    const std::string repo = sb.project();
    write_content(repo, "a.txt", "one\n");
    write_content(repo, "b.txt", "two\n");
    write_content(repo, "gone.txt", "three\n");

    UndoStack stack;
    const Snapshot level = take(repo, sb.store());
    ASSERT_TRUE(level.kind == Kind::DirCopy);
    stack.push(level);

    /* Работа шага: правка, создание и удаление разом. */
    write_content(repo, "a.txt", "ONE\n");
    write_content(repo, "b.txt", "TWO\n");
    fs::remove(fs::path(repo) / "gone.txt", ec);
    write_content(repo, "fresh.txt", "four\n");

    const MoveResult u = stack.undo(repo, sb.store());
    ASSERT_TRUE(u.ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("one\n"));
    ASSERT_EQ(read_text(fs::path(repo) / "b.txt"), std::string("two\n"));
    ASSERT_TRUE(fs::exists(fs::path(repo) / "gone.txt"));
    /* Созданный после уровня файл ПЕРЕЖИВАЕТ отмену (отклонение 122), и
     * инструмент обязан сказать об этом словами — иначе модель сочтёт
     * проект приведённым к состоянию. */
    ASSERT_TRUE(fs::exists(fs::path(repo) / "fresh.txt"));

    ASSERT_EQ(u.restored.size(), (size_t)3);
    bool named_a = false, named_b = false, named_gone = false;
    for (const std::string& f : u.restored) {
        if (f == "a.txt") named_a = true;
        if (f == "b.txt") named_b = true;
        if (f == "gone.txt") named_gone = true;
    }
    ASSERT_TRUE(named_a);
    ASSERT_TRUE(named_b);
    ASSERT_TRUE(named_gone);
    ASSERT_TRUE(u.source.find("копия") != std::string::npos);
    /* Лево остаётся в ответе: файла, который вернуть нельзя, отмена не
     * создаёт, но сказать о нём обязана — иначе «проект приведён к
     * состоянию» было бы неправдой. */
    ASSERT_EQ(u.leftover.size(), (size_t)0);
    ASSERT_TRUE(u.can_redo);
    /* Возврат вперёд ПОСЛЕ ОТМЕНЫ, ОСТАВИВШЕЙ ФАЙЛ, РАБОТАЕТ (отклонение
     * 122 закрыто): сравнение идёт с состоянием, которое оставил отменённый
     * уровень, а не с состоянием уровня. До починки сравнение шло с
     * уровнем, файл `fresh.txt` в него не входил, и отказ был неизбежен —
     * то есть на не-git-проекте вернуться вперёд после отмены было нельзя
     * никогда, а на git-проекте можно (там состояние — tree-hash, и
     * неотслеживаемый файл в него не входит).
     *
     * Поведение закреплено проверкой с обеих сторон: раньше здесь стоял
     * `ASSERT_FALSE(back.ok)`, и починка обязана была перевернуть именно
     * эту строку, а не добавить рядом вторую проверку. */
    const MoveResult back = stack.redo(repo, sb.store());
    ASSERT_TRUE(back.ok);
    /* Откат не удаляет созданный файл — и на ОТКАТЕ НАЗАД, тоже: возврат
     * вперёд приводит файлы к состоянию уровня, а `fresh.txt` в этом
     * состоянии не было. Он остаётся, и это сказано словами. */
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("ONE\n"));
    ASSERT_EQ(read_text(fs::path(repo) / "b.txt"), std::string("TWO\n"));
    ASSERT_TRUE(fs::exists(fs::path(repo) / "gone.txt"));
    ASSERT_TRUE(fs::exists(fs::path(repo) / "fresh.txt"));
    /* И назад: возврат вперёд не сломал отмену. */
    const MoveResult again = stack.undo(repo, sb.store());
    ASSERT_TRUE(again.ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("one\n"));
    ASSERT_EQ(read_text(fs::path(repo) / "b.txt"), std::string("two\n"));
    /* Стек после всего этого остался пригоден, и стоит он на самом раннем
     * состоянии: следующая отмена обязана ОТКАЗАТЬ, а не отменить уже
     * отменённый шаг (файл `fresh.txt`, переживший отмену, до починки
     * делал ровно это — см. вторую половину проверки ниже). */
    const MoveResult early = stack.undo(repo, sb.store());
    ASSERT_FALSE(early.ok);
    ASSERT_TRUE(early.reason.find("самое раннее") != std::string::npos);
}

TEST(two_undos_in_a_row_each_cancel_exactly_one_step_when_a_file_survived_the_first) {
    /* ТО ЖЕ, ЧТО ПЕРВАЯ ПОЛОВИНА, НО НА ТРЁХ УРОВНЯХ: два отменённых
     * шага подряд, и на каждом шаге остаётся файл, который отмена не
     * уносит.
     *
     * Здесь ломается ровно то, что не видит проверка выше. Отмена первого
     * шага оставила `new1.txt`; второе нажатие «отменить» сравнивало проект
     * с УРОВНЕМ, видело файл, которого в уровне нет, и считало, что проект
     * упирается вперёд, — то есть отменяло тот же первый шаг вместо
     * второго. На третьем нажатии позиция уехала в начало сессии.
     *
     * Проверяется не «отмена вообще работает», а ЧТО ИМЕННО ОТМЕНЕНО на
     * каждом нажатии: файлы шага. Граница названа: сравнение ведётся с
     * состоянием, которое оставил переход, и для этого состояния снимается
     * свой снимок — на не-git-проекте это копия каталога, и она снимается
     * только когда отличается от уровня (шапка UndoStack). */
    SandBox sb("undo_copy_two_steps");
    std::error_code ec;
    const std::string repo = sb.project();
    write_content(repo, "a.txt", "v0\n");

    UndoStack stack;
    stack.push(take(repo, sb.store()));       /* уровень 0 */

    write_content(repo, "a.txt", "v1\n");
    write_content(repo, "new1.txt", "n1\n");
    stack.push(take(repo, sb.store()));       /* уровень 1 */

    write_content(repo, "a.txt", "v2\n");
    write_content(repo, "new2.txt", "n2\n");
    stack.push(take(repo, sb.store()));       /* уровень 2 */

    ASSERT_TRUE(stack.undo(repo, sb.store()).ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("v1\n"));
    /* Второе нажатие отменяет ВТОРОЙ шаг, а не первый повторно. */
    ASSERT_TRUE(stack.undo(repo, sb.store()).ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("v0\n"));
    /* Оба переживших файла на месте: откат не удаляет (отклонение 122). */
    ASSERT_TRUE(fs::exists(fs::path(repo) / "new1.txt"));
    ASSERT_TRUE(fs::exists(fs::path(repo) / "new2.txt"));
    /* Третье нажатие — отказ, а не «отмена уровня 0 в третий раз». */
    ASSERT_FALSE(stack.undo(repo, sb.store()).ok);
    /* И обратно вперёд на два шага: возврат перескакивает через
     * пережившие файлы, а не упирается в них. */
    ASSERT_TRUE(stack.redo(repo, sb.store()).ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("v1\n"));
    ASSERT_TRUE(stack.redo(repo, sb.store()).ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("v2\n"));
    /* Копии, снятые ради состояния «после отката», не остаются лежать: их
     * отдаёт take_trash, и на не-git-проекте это каталоги (отклонение
     * 109). Проверяется то, что доступно: стек жив и отмена работает. */
    /* Хеш цели в ответе читается ПОСЛЕ того, как стек расширился отменённым
     * состоянием. Пока move() держал ссылку на элемент levels_ и читал её
     * после push_back, это было чтение освобождённой памяти: на мусоре
     * проверка не падала, а на переиспользованном буфере — сегфолтом.
     * Поэтому сверяется ЗНАЧЕНИЕ, а не «не упало». */
    const MoveResult last = stack.undo(repo, sb.store());
    ASSERT_TRUE(last.ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("v1\n"));
    ASSERT_FALSE(last.level_hash.empty());
    ASSERT_EQ(last.level_hash, stack.level(1).hash);
    (void)ec;
}

TEST(undo_then_redo_works_on_a_copy_snapshot_when_nothing_was_created) {
    /* Вторая половина предыдущей проверки, без её оговорки: если отменённый
     * шаг не СОЗДАВАЛ файл, отмена приводит проект к уровню точно, и
     * возврат вперёд работает. Этот случай обязателен отдельно: одна
     * проверка с отказом redo ничего не говорит о том, что обычный
     * откат-вернуть-вернуть работает, а сломать его правкой правила
     * нельзя было бы незаметно. */
    SandBox sb("undo_copy_redo");
    std::error_code ec;
    const std::string repo = sb.project();
    write_content(repo, "a.txt", "one\n");
    write_content(repo, "b.txt", "two\n");

    UndoStack stack;
    stack.push(take(repo, sb.store()));
    write_content(repo, "a.txt", "ONE\n");
    write_content(repo, "b.txt", "TWO\n");

    ASSERT_TRUE(stack.undo(repo, sb.store()).ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("one\n"));
    ASSERT_EQ(read_text(fs::path(repo) / "b.txt"), std::string("two\n"));
    const MoveResult back = stack.redo(repo, sb.store());
    ASSERT_TRUE(back.ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("ONE\n"));
    ASSERT_EQ(read_text(fs::path(repo) / "b.txt"), std::string("TWO\n"));
    /* И назад: три файла отменены одним нажатием, три вернулись. */
    ASSERT_TRUE(stack.undo(repo, sb.store()).ok);
    ASSERT_EQ(read_text(fs::path(repo) / "a.txt"), std::string("one\n"));
    ASSERT_EQ(read_text(fs::path(repo) / "b.txt"), std::string("two\n"));
}

TEST(an_interrupted_step_leaves_the_stack_and_the_snapshots_whole) {
    /* ПРЕРЫВАНИЕ. Проверяется то, что снимковый строй обязан пережить:
     * шаг начался (снимок и уровень уже есть), агент был прерван ДО
     * того, как что-то сделал, и следующая задача сняла свой снимок.
     *
     * Что здесь ломается, если строй неверен, и как это выглядит:
     *   - уровень не появился бы → отменять нечего, и человек после
     *     прерывания не смог бы вернуть последнее сделанное;
     *   - два одинаковых уровня подряд → «отмена» жала бы вхолостую;
     *   - last_snapshot потерял бы состояние → пустой hash у revert
     *     вернул бы проект вперёд, к началу шага (отклонение 118);
     *   - копия уровня осталась бы в каталоге данных и не была бы
     *     удалена никем (отклонение 109).
     *
     * Граница названа: прерывание модели и прерывание записи НЕ
     * моделируются. Проверяется целостность ПОСЛЕ того, как шаг снял
     * снимок, — то есть ровно та граница, на которой снимок и появляется. */
    EngineState& state = engine_state();
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        state.project_dir.clear();
        state.last_snapshot = Snapshot();
        state.undo_stack.clear();
    }
    drain_stack_trash(state);

    SandBox sb("interrupted");
    const std::string repo = sb.project();
    write_content(repo, "a.txt", "one\n");

    HostCallbacks cb;
    cb.path_data_dir = [&sb] { return sb.store(); };
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        state.project_dir = repo;
    }

    /* Шаг начался: снимок снят, уровень заведён. Дальше агента прервали
     * до первого вызова инструмента — то есть файлов он не менял. */
    const StepSnapshot first = take_step_snapshot(state, cb, nullptr);
    ASSERT_TRUE(first.after.ok());
    /* `discard` — контракт вызывающего (шапка StepSnapshot): снимок, не
     * ставший уровнем, удаляется ПОСЛЕ того, как вызывающий прочитал его
     * через changed_files. Проверка его не убирала, и копия оставалась
     * лежать — то есть проверка сама была причиной утечки, которую искала. */
    discard_copies(first.discard);
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        ASSERT_EQ(state.undo_stack.size(), (size_t)1);
        ASSERT_EQ(state.undo_stack.level(0).hash, first.after.hash);
        /* last_snapshot — состояние на начало шага, то есть тот же
         * снимок: без этого пустой hash у revert означал бы «начало
         * ТЕКУЩЕГО шага» не из того состояния. */
        ASSERT_EQ(state.last_snapshot.hash, first.after.hash);
    }

    /* Следующая задача после прерывания: состояние не менялось, значит
     * и уровень не добавляется — иначе «отмена» жала бы вхолостую. */
    const StepSnapshot second = take_step_snapshot(state, cb, nullptr);
    ASSERT_TRUE(second.after.ok());
    discard_copies(second.discard);
    size_t copies_before;
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        ASSERT_EQ(state.undo_stack.size(), (size_t)1);
        ASSERT_EQ(state.undo_stack.position(), (size_t)0);
        /* last_snapshot не сдвинулся на «позже»: он и должен называть
         * начало текущего шага. */
        ASSERT_EQ(state.last_snapshot.hash, second.after.hash);
        copies_before = copy_dirs_in_store(sb.store());
    }
    ASSERT_EQ(copies_before, (size_t)1);

    /* Агент всё же поработал, его прервали, и человек нажимает «отменить»
     * через инструмент — стек обязан быть пригоден после прерывания. */
    write_content(repo, "a.txt", "one\ntwo\n");
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        state.undo_stack.clear();
    }
    drain_stack_trash(state);
    write_content(repo, "a.txt", "one\n");
    const StepSnapshot after_work = take_step_snapshot(state, cb, nullptr);
    ASSERT_TRUE(after_work.after.ok());
    discard_copies(after_work.discard);
    write_content(repo, "a.txt", "правка после прерывания\n");
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        state.undo_stack.push(after_work.after);
        ASSERT_EQ(state.undo_stack.size(), (size_t)1);
    }
    /* Каталог копий — ТОТ ЖЕ, что у движка (store_dir от data_dir).
     * Первая версия звала diff_step с sb.store() напрямую, и тогда
     * снимок, снятый ради сравнения, ложился на уровень глубже, чем
     * снимки движка; проверка считала только каталог движка и утечку не
     * видела — то есть мутация «не убирать копию после diff» выживала
     * не потому, что утечки нет, а потому, что смотрели не туда. */
    const DiffReport what_will_change = [&] {
        std::lock_guard<std::mutex> lk(state.mtx);
        return state.undo_stack.diff_step(0, repo, store_dir(sb.store()));
    }();
    ASSERT_TRUE(what_will_change.ok);
    ASSERT_EQ(what_will_change.files.size(), (size_t)1);

    {
        std::lock_guard<std::mutex> lk(state.mtx);
        state.project_dir.clear();
        state.last_snapshot = Snapshot();
        state.undo_stack.clear();
    }
    drain_stack_trash(state);
    /* После уборки в каталоге копий ничего не осталось: уровень был
     * один, и его копия ушла вместе с ним. Мусор, который никто не
     * удалил, копился бы от прерывания к прерыванию. */
    ASSERT_EQ(copy_dirs_in_store(sb.store()), (size_t)0);
}
