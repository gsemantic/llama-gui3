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

    const Snapshot s = take_step_snapshot(state, cb, nullptr);
    if (!s.ok())    ASSERT_TRUE(s.ok());
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        ASSERT_TRUE(state.last_snapshot.ok());
        ASSERT_EQ(state.last_snapshot.hash, s.hash);
        ASSERT_EQ(state.snapshots_taken, 1);
    }
    /* Второй шаг — вторая запись: счётчик обязан расти, иначе «снимки
     * берутся» останется утверждением без доказательства. */
    take_step_snapshot(state, cb, nullptr);
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
