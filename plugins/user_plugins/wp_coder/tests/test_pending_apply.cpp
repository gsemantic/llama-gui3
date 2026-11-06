/*
 * test_pending_apply.cpp — И13.2 (D7): кнопка «применить» пишет файл
 * ТЕМ ЖЕ ПУТЕМ, что и инструмент записи.
 *
 * До И13.2 в `Engine::pending_apply` не было ни одной проверки пути. Путь
 * приходил от модели, в режиме плана инструмент отдавал его в
 * `propose_write` ДО своих проверок, а кнопка писала его как есть. То есть
 * агент мог предложить `../../etc/cron.d/x` или путь, ушедший по симлинку за
 * пределы проекта, и человек, нажавший «применить», писал файл туда.
 *
 * Форма проверок — по одному случаю на КАЖДУЮ ветвь решения, плюс сквозная
 * проверка через настоящий инструмент в режиме плана: только она доказывает,
 * что дыра закрыта там, где была, а не в копии того же кода.
 *
 * Отказ обязан быть ВИДИМ. Человек нажал кнопку; если после отказа ничего не
 * написано и ничего не сказано, кнопка выглядит сломанной, а предложение
 * пропало — и повторить правку агент уже не сможет. Поэтому проверки смотрят
 * и на текст события, и на то, что предложение осталось в списке.
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/engine.h"
#include "../core/base_tools.h"
#include "../core/project.h"
#include "../core/permission_engine.h"
#include "../core/json.h"
#include "../core/tools_registry.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace coder;

namespace {

fs::path make_tmp_dir() {
    fs::path tmp = fs::temp_directory_path()
        / ("wp_coder_pending_" + std::to_string(::getpid()) + "_"
           + std::to_string(std::rand()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    register_tmp_tree(tmp.string());
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

/* Снимок состояния до правок и возврат по нему: движок — синглтон, иначе
 * проект и правила уедут в следующий тест (правило реентерабельности). */
struct Fixture {
    std::string project_dir;
    std::vector<std::string> allowed_external;
    bool plan_mode = false;
    /* `project` — корень проекта, `root` — каталог фикстуры целиком.
     *
     * Разделены намеренно. Каталоги ВНЕ проекта нужны проверкам правил, и
     * брать их по соседству фикстуры нельзя: путь вроде `/tmp/shared/x`
     * одинаков в каждом прогоне, поэтому следы прошлого прогона делали бы
     * следующий зелёным или красным по памяти, а не по коду (грабля 10 в
     * применении к тестам). */
    fs::path root;
    fs::path project;

    explicit Fixture(bool with_plan_mode) : root(make_tmp_dir()) {
        project = root / "project";
        fs::create_directories(project);
        {
            std::lock_guard<std::mutex> lk(engine_state().mtx);
            project_dir = engine_state().project_dir;
            plan_mode = engine_state().plan_mode;
            allowed_external = engine_state().allowed_external_paths;
        }
        HostCallbacks cb;
        cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&,
                         LlmReply&) { return false; };
        cb.llm_complete = [](const std::string&, const std::string&,
                             std::string&) { return false; };
        cb.llm_is_connected = []() { return false; };
        cb.chat_event = [](const std::string&) {};
        /* Каталог данных — тот же каталог фикстуры: снимки пишутся мимо
         * проекта, и иначе они осели бы в /tmp до конца прогона. */
        cb.path_data_dir = [this]() -> std::string { return project.string(); };
        Engine::instance().init(cb);
        Engine::instance().permissions().reset();
        write_file(project / "a.php", "<?php\n");
        {
            std::lock_guard<std::mutex> lk(engine_state().mtx);
            engine_state().project_dir = project.string();
            engine_state().allowed_external_paths.clear();
            engine_state().plan_mode = with_plan_mode;
            engine_state().pending.clear();
            engine_state().events.clear();
            engine_state().undo_stack.clear();
            engine_state().last_snapshot = snapshot::Snapshot();
        }
        register_base_tools();
    }
    ~Fixture() {
        {
            std::lock_guard<std::mutex> lk(engine_state().mtx);
            engine_state().project_dir = project_dir;
            engine_state().plan_mode = plan_mode;
            engine_state().allowed_external_paths = allowed_external;
            engine_state().pending.clear();
            engine_state().undo_stack.clear();
        }
        Engine::instance().permissions().reset();
    }
};

/* Последнее событие, начинающееся с метки, — тем оно и интересно: отказ
 * обязан быть ПОСЛЕДНИМ, иначе его перебило бы что-то другое и человек не
 * увидел бы причину. */
std::string last_event(const std::string& marker) {
    std::string found;
    for (const AgentEvent& e : engine().events()) {
        if (e.text.find(marker) == 0) found = e.text;
    }
    return found;
}

size_t pending_count() {
    std::lock_guard<std::mutex> lk(engine_state().mtx);
    return engine_state().pending.size();
}

void propose(const std::string& path, const std::string& content) {
    std::lock_guard<std::mutex> lk(engine_state().mtx);
    engine_state().pending.push_back({path, content});
}

std::string run_write(const std::string& path, const std::string& content) {
    json::JsonValue args = json::JsonValue::object();
    args.set("path", path);
    args.set("content", content);
    return ToolsRegistry::instance().run("write_file", args);
}

} // anonymous namespace

/* ======================================================================
 * Положительный контроль: обычная правка применяется
 * ====================================================================== */

TEST(pending_apply_writes_a_safe_path) {
    Fixture fx(false);
    propose("a.php", "<?php applied\n");
    engine().pending_apply(0);
    ASSERT_EQ(read_file(fx.project / "a.php"), std::string("<?php applied\n"));
    ASSERT_EQ(pending_count(), size_t(0));
    ASSERT_TRUE(last_event("[применить] записано").find("a.php") !=
                std::string::npos);
}

/* Предложение, отказ по которому НЕ сработал, не должно было применяться
 * вообще — но проверяется не это, а обратное: обычная правка не ломается
 * добавленными проверками (иначе «защита» была бы просто запретом писать). */
TEST(pending_apply_keeps_writing_ordinary_files) {
    Fixture fx(false);
    fs::create_directories(fx.project / "wp-content" / "themes");
    propose("wp-content/themes/style.css", "body{}\n");
    engine().pending_apply(0);
    ASSERT_EQ(read_file(fx.project / "wp-content" / "themes" / "style.css"),
              std::string("body{}\n"));
}

/* ======================================================================
 * Отказ по пути
 * ====================================================================== */

/* Сквозная проверка D7: настоящий инструмент в режиме плана, настоящее
 * предложение, настоящая кнопка. Именно этот путь и был дырой — модель
 * предлагает путь, который никто не проверил, и человек его применяет. */
TEST(plan_mode_proposal_of_a_traversal_path_is_never_applied) {
    Fixture fx(true);
    const std::string evil = run_write("../escaped.php", "<?php evil\n");
    /* Инструмент согласился предложить правку — это его работа в режиме
     * плана, и проверка не про то, чтобы он отказал. */
    ASSERT_TRUE(evil.find("[предложено") != std::string::npos);
    ASSERT_EQ(pending_count(), size_t(1));

    engine().pending_apply(0);
    ASSERT_FALSE(fs::exists(fx.root / "escaped.php"));
    ASSERT_EQ(read_file(fx.project / "a.php"), std::string("<?php\n"));
    /* Предложение ОСТАЛОСЬ: отказ по пути не должен уносить чужую работу. */
    ASSERT_EQ(pending_count(), size_t(1));
    const std::string said = last_event("[применить]");
    ASSERT_TRUE(said.find("НЕ применена") != std::string::npos);
    ASSERT_TRUE(said.find("небезопасный путь") != std::string::npos);
}

/* Путь, ушедший по симлинку, — тест И13.1 под другим углом: проверка пути
 * живёт в guard'е, а кнопка «применить» обязана этим guard'ем пользоваться. */
TEST(pending_apply_refuses_a_path_that_leaves_the_project_by_symlink) {
    Fixture fx(false);
    fs::create_directories(fx.root / "outside");
    std::error_code ec;
    fs::create_symlink(fx.root / "outside",
                       fx.project / "out", ec);
    propose("out/stolen.php", "<?php evil\n");
    engine().pending_apply(0);
    ASSERT_FALSE(fs::exists(fx.root / "outside" / "stolen.php"));
    ASSERT_EQ(pending_count(), size_t(1));
    ASSERT_TRUE(last_event("[применить]").find("вне проекта") !=
                std::string::npos);
}

/* Запрещённый системный каталог. Материал проверяется ПЕРВОЙ: если бы
 * `/dev/shm` оказался недоступен для записи, тест был бы зелёным и при
 * коде без проверки — то есть доказывал бы пустоту (правило 8). */
TEST(pending_apply_refuses_a_system_directory) {
    Fixture fx(false);
    const fs::path probe = fs::path("/dev/shm") /
                           ("wp_coder_pending_probe_" + std::to_string(::getpid()));
    {
        std::ofstream f(probe, std::ios::binary | std::ios::trunc);
        if (!f) {
            std::cerr << "  /dev/shm недоступен для записи — материал проверки"
                         " непригоден, тест ничего не доказывает" << std::endl;
            ASSERT_TRUE(false);
        }
        f << "probe\n";
    }
    fs::remove(probe);

    propose(probe.string(), "<?php evil\n");
    engine().pending_apply(0);
    ASSERT_FALSE(fs::exists(probe));
    ASSERT_EQ(pending_count(), size_t(1));
    ASSERT_TRUE(last_event("[применить]").find("запись запрещена") !=
                std::string::npos);
}

/* Внешний путь, на который ПРАВИЛА разрешают, применяется: запрет всего
 * вне проекта сломал бы плановый режим для каталога данных и /tmp, то есть
 * убрал бы возможность, которой в обычном режиме есть. */
TEST(pending_apply_obeys_the_rules_about_paths_outside_the_project) {
    Fixture fx(false);
    const fs::path outside = fx.root / "shared" / "config.php";
    fs::create_directories(outside.parent_path());

    /* Путь АБСОЛЮТНЫЙ, и это не оформление: относительный путь
     * разрешается от корня проекта и потому принципиально не может выйти
     * за него — материал проверки просто не был бы материалом. */
    /* Правила молчат (Ask) — применяться нечему. */
    propose(outside.string(), "a=1\n");
    engine().pending_apply(0);
    ASSERT_FALSE(fs::exists(outside));
    ASSERT_EQ(pending_count(), size_t(1));

    /* То же правило, разрешающее этот каталог, — применяется. */
    Rule allow;
    allow.permission = "external_directory";
    allow.pattern = (fx.root / "shared" / "*").string();
    allow.action = PermissionAction::Allow;
    Engine::instance().permissions().add_rule(allow);
    engine().pending_apply(0);
    ASSERT_EQ(read_file(outside), std::string("a=1\n"));
    ASSERT_EQ(pending_count(), size_t(0));
}

/* Отказ не должен унести предложение и не должен записать ПОЛОВУ: тот же
 * путь, на который отказали из-за правила, обязан применяться после снятия
 * правила — то есть проверка стоит ДО записи, а не после. */
TEST(pending_apply_refuses_before_writing_anything) {
    Fixture fx(false);
    const fs::path outside = fx.root / "shared" / "data.txt";
    fs::create_directories(outside.parent_path());
    propose(outside.string(), "payload\n");
    engine().pending_apply(0);
    ASSERT_FALSE(fs::exists(outside));

    Rule allow;
    allow.permission = "external_directory";
    allow.pattern = (fx.root / "shared" / "*").string();
    allow.action = PermissionAction::Allow;
    Engine::instance().permissions().add_rule(allow);
    engine().pending_apply(0);
    ASSERT_EQ(read_file(outside), std::string("payload\n"));
}

/* Отказ по НЕСУЩЕСТВУЮЩЕЙ причине (каталога нет) — тоже говорит словами.
 * Без этого отличить «защита сработала» от «не смогли записать» нельзя,
 * и человек решил бы, что кнопка сломана. */
TEST(pending_apply_says_why_the_file_was_not_written) {
    Fixture fx(false);
    propose("нет/такого/каталога/new.php", "<?php\n");
    engine().pending_apply(0);
    ASSERT_EQ(pending_count(), size_t(1));
    const std::string said = last_event("[применить]");
    ASSERT_TRUE(said.find("НЕ применена") != std::string::npos);
    ASSERT_TRUE(said.find("не удалось записать") != std::string::npos);
}

/* ======================================================================
 * Откат: правка по кнопке обязана быть уровнем сессии
 * ====================================================================== */

/* И13.2: `.orig`-бэкапы выпилены И10.3 (отклонение 116), и на их месте
 * уровни сессии. Правка, применённая человеком, обязана быть в них видима —
 * иначе «применить» остаётся необратимой операцией, а это ровно то, чего
 * требовал текст задачи под именем бэкапа. */
TEST(pending_apply_makes_the_write_a_level_of_the_session) {
    Fixture fx(false);
    size_t before = 0;
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        before = engine_state().undo_stack.size();
    }
    propose("a.php", "<?php applied\n");
    engine().pending_apply(0);
    std::lock_guard<std::mutex> lk(engine_state().mtx);
    ASSERT_TRUE(engine_state().undo_stack.size() > before);
}