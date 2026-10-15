/*
 * test_path_guard.cpp — И13.1 (D6): «лежит ли этот путь внутри проекта».
 *
 * Проверяется не «функция существует», а РЕШЕНИЕ о конкретных путях на живой
 * файловой системе: симлинки создаются по-настоящему, каталоги — по-настоящему,
 * и ответ читается с диска. Причина именно такая: D6 жил ровно в том, что
 * решение принималось по СТРОКЕ, а не по тому, куда файл попадёт.
 *
 * Форма проверок — таблица с ожидаемым вердиктом по каждому пути плюс
 * утверждение о ДЕФЕКТЕ: фикстура проверяется старым строковым правилом, и
 * если оно тоже сказало бы «внутри», проверка доказывала бы пустоту. Иначе
 * тест зелен на любом коде, который просто возвращает false.
 *
 * Два слоя:
 *   1. Решение функции (is_path_outside / is_path_allowed) — быстро, без
 *      разрешений.
 *   2. Два ПОТРЕБИТЕЛЯ: PermissionGate и инструмент `write_file`. Слой 1
 *      без слоя 2 — ровно класс «механизм есть ≠ механизм работает»: функции
 *      могли бы быть правильными, а вызывающий — смотреть в другое место.
 *
 * Чего эти проверки НЕ делают: они не проверяют, что симлинк нельзя
 * создать. Защита от выхода за пределы проекта — не запрет создавать
 * симлинки (opencode такого запрета и не имеет); здесь важно, что путь,
 * ушедший по симлинку наружу, перестаёт считаться своим.
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/security.h"
#include "../core/engine.h"
#include "../core/base_tools.h"
#include "../core/project.h"
#include "../core/permission_engine.h"
#include "../core/agent_components.h"
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
using coder::security::is_path_allowed;
using coder::security::is_path_outside;

namespace {

/* Фикстура: корень проекта, каталог СНАРУЖИ и симлинк из проекта в него.
 *
 * Симлинк смотрит на СУЩЕСТВУЮЩИЙ каталог: `weakly_canonical` разворачивает
 * только существующую часть пути, и висячий симлинк прошёл бы мимо любой
 * проверки на ФС. Это не мелочь — половина «проверок симлинков» в проектах
 * проверяет ровно висячий симлинк и потому зелёная на коде без защиты. */
struct Tree {
    fs::path root;      /* корень проекта */
    fs::path outside;   /* то, куда ведёт симлинк наружу */
    std::string escape; /* текстом внутри проекта, на диске снаружи */
};

fs::path make_tmp_dir() {
    fs::path tmp = fs::temp_directory_path()
        / ("wp_coder_guard_" + std::to_string(::getpid()) + "_"
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

Tree make_tree() {
    fs::path tmp = make_tmp_dir();
    Tree t;
    t.root = tmp / "project";
    t.outside = tmp / "outside";
    fs::create_directories(t.root / "wp-content" / "themes");
    fs::create_directories(t.outside);
    write_file(t.root / "a.php", "<?php\n");
    write_file(t.root / "wp-content" / "themes" / "style.css", "body{}\n");
    std::error_code ec;
    fs::create_symlink(t.outside, t.root / "escape", ec);
    t.escape = (t.root / "escape" / "stolen.php").string();
    return t;
}

/* Ровно то правило, которое стояло до И13.1: строковый префикс с
 * приведением `//` и `/.`. Оно живёт здесь НЕ как эталон реализации, а как
 * утверждение о дефекте: проверка обязана убедиться, что её материал —
 * именно тот случай, который старое правило пропускало. Если материал вдруг
 * перестанет быть таким, проверка ничего не доказывает и должна это сказать. */
bool old_string_rule_says_inside(const std::string& abs_path,
                                const std::string& project_dir) {
    auto normalize = [](const std::string& s) -> std::string {
        std::string r;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') continue;
            if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '.') {
                if (i + 2 >= s.size() || s[i + 2] == '/') { i += 1; continue; }
            }
            r += s[i];
        }
        return r;
    };
    std::string norm_path = normalize(abs_path);
    std::string norm_root = normalize(project_dir);
    if (!norm_root.empty() && norm_root.back() != '/') norm_root += '/';
    return norm_path.find(norm_root) == 0 || norm_path == normalize(project_dir);
}

/* Движок и правила разрешений под конкретный проект. Правило на
 * `external_directory` — Deny, а не Ask: PermissionGate::check ждать не
 * ждёт, но Ask оставил бы следующий за ним прогон в состоянии
 * WaitingPermission, и падение пришлось бы читать как дефект пути.
 *
 * Снимок состояния снимается ДО правок и возвращается в деструкторе: движок —
 * синглтон, и оставленные здесь проект и правила уезжают в следующий тест
 * (правило реентерабельности). Правила тоже сбрасываются — так же, как это
 * делает PermissionGuard в test_permission_engine.cpp. */
struct ProjectFixture {
    std::string project_dir;
    std::vector<std::string> allowed_external;
    bool plan_mode = false;

    explicit ProjectFixture(const fs::path& project) {
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
        Engine::instance().init(cb);
        Engine::instance().permissions().reset();
        Rule deny_external;
        deny_external.permission = "external_directory";
        deny_external.pattern = "*";
        deny_external.action = PermissionAction::Deny;
        deny_external.comment = "тест: наружу не ходим";
        Engine::instance().permissions().add_rule(deny_external);
        {
            std::lock_guard<std::mutex> lk(engine_state().mtx);
            engine_state().project_dir = project.string();
            engine_state().allowed_external_paths.clear();
            engine_state().plan_mode = false;
            engine_state().pending.clear();
        }
        register_base_tools();
    }
    ~ProjectFixture() {
        {
            std::lock_guard<std::mutex> lk(engine_state().mtx);
            engine_state().project_dir = project_dir;
            engine_state().plan_mode = plan_mode;
            engine_state().allowed_external_paths = allowed_external;
            engine_state().pending.clear();
        }
        Engine::instance().permissions().reset();
    }
};

std::string run_write(const std::string& path, const std::string& content) {
    json::JsonValue args = json::JsonValue::object();
    args.set("path", path);
    args.set("content", content);
    return ToolsRegistry::instance().run("write_file", args);
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

bool exists(const std::string& p) {
    std::error_code ec;
    return fs::exists(fs::path(p), ec);
}

} // anonymous namespace

/* ======================================================================
 * 1. Решение функции
 * ====================================================================== */

/* Симлинк наружу — сам D6. Проверка обязана убедиться, что материал
 * подходит: старое строковое правило считало этот путь своим. */
TEST(path_guard_rejects_symlink_out_of_project) {
    Tree t = make_tree();
    ASSERT_TRUE(old_string_rule_says_inside(t.escape, t.root.string()));
    ASSERT_TRUE(is_path_outside(t.escape, t.root.string()));
}

/* Положительный контроль на том же материале: соседний каталог с тем же
 * началом строки — снаружи, и никакой симлинк в нём не нужен. */
TEST(path_guard_rejects_sibling_with_the_same_prefix) {
    Tree t = make_tree();
    const std::string sibling = (t.root.string() + "-backup/notes.txt");
    ASSERT_FALSE(is_path_outside((t.root / "a.php").string(), t.root.string()));
    ASSERT_TRUE(is_path_outside(sibling, t.root.string()));
}

/* `..` внутри текста проекта. Старое правило сворачивало только `//` и
 * `/.`: строка `/проект/../etc/passwd` начиналась с `/проект/`, то есть
 * считалась своей. Это вторая, не символическая, дыра того же места. */
TEST(path_guard_resolves_dotdot_instead_of_reading_the_text) {
    Tree t = make_tree();
    const std::string via_dotdot = (t.root / ".." / "outside" / "x.php").string();
    ASSERT_TRUE(old_string_rule_says_inside(via_dotdot, t.root.string()));
    ASSERT_TRUE(is_path_outside(via_dotdot, t.root.string()));
    /* Обратное направление — тоже: `..` не должен выкидывать наружу то,
     * что внутри. */
    const std::string inside = (t.root / "wp-content" / ".." / "a.php").string();
    ASSERT_TRUE(old_string_rule_says_inside(inside, t.root.string()));
    ASSERT_FALSE(is_path_outside(inside, t.root.string()));
}

/* Корень проекта сам reached через симлинк. Если канонизировать только
 * проверяемый путь, такой проект объявил бы наружу ВСЁ, и проверка
 * превратилась бы в «любой проект под симлинком сломан». */
TEST(path_guard_accepts_a_project_reached_through_a_symlink) {
    Tree t = make_tree();
    const fs::path linked_root = t.root.parent_path() / "linked_root";
    std::error_code ec;
    fs::create_symlink(t.root, linked_root, ec);
    ASSERT_FALSE(is_path_outside((linked_root / "a.php").string(),
                                 linked_root.string()));
    /* Наружу из такого проекта — по-прежнему наружу: симлинк `escape`
     * разворачивается и здесь. */
    ASSERT_TRUE(is_path_outside((linked_root / "escape" / "stolen.php").string(),
                                linked_root.string()));
}

/* Несуществующий файл: запись нового файла обязана проходить, поэтому
 * канонизация обязана быть `weakly_canonical`, а не `canonical` —
 * `canonical` на несуществующем пути вернул бы пустой путь и тихо
 * превратил бы «внутри» в «снаружи». */
TEST(path_guard_accepts_a_file_that_does_not_exist_yet) {
    Tree t = make_tree();
    const std::string fresh = (t.root / "new" / "deep" / "fresh.php").string();
    ASSERT_FALSE(exists(fresh));
    ASSERT_FALSE(is_path_outside(fresh, t.root.string()));
    /* Несуществующий каталог НАРУЖИ — снаружи, и тоже без вопросов. */
    ASSERT_TRUE(is_path_outside((t.root / ".." / "нет" / "нет.php").string(),
                                t.root.string()));
}

/* Петля симлинков — путь, который не разрешить. Доказать, что он внутри,
 * не удалось, значит считать его своим нельзя. */
TEST(path_guard_treats_an_unresolvable_path_as_external) {
    Tree t = make_tree();
    const fs::path loop_a = t.root / "loopA";
    const fs::path loop_b = t.root.parent_path() / "loopB";
    std::error_code ec;
    fs::create_symlink(loop_b, loop_a, ec);
    fs::create_symlink(loop_a, loop_b, ec);
    ASSERT_TRUE(is_path_outside((loop_a / "x.php").string(), t.root.string()));
    /* Корень проекта, который не разрешить, — с тем же смыслом: про
     * такой проект сказать «внутри» нельзя. */
    ASSERT_TRUE(is_path_outside((t.root / "a.php").string(),
                                (loop_b).string()));
}

/* Пустой корень проекта — проверять нечего. Проект без каталога не должен
 * спрашивать разрешение на каждый файл, иначе сломается всё, кроме
 * безопасности. */
TEST(path_guard_without_project_root_asks_nothing) {
    Tree t = make_tree();
    ASSERT_FALSE(is_path_outside(t.escape, ""));
    ASSERT_FALSE(is_path_outside("/etc/passwd", ""));
}

/* Обратная пустота: путь, которого нет, — не «свой», даже при заданном
 * корне. Здесь важен не столько ответ, сколько то, что функция обязана его
 * дать, не разыменовывая конец пути: относительного пути у пустого кандидата
 * не существует, и проверка обязана остановиться на этом ДО `*begin()`. */
TEST(path_guard_rejects_an_empty_candidate_path) {
    Tree t = make_tree();
    ASSERT_TRUE(is_path_outside("", t.root.string()));
    /* Список разрешённого — последнее слово, и шаблон `*` покрывает всё,
     * включая пустой путь: так устроен `is_path_allowed`, и это поведение
     * И13.1 не менял. С непохожим шаблоном — не разрешён. */
    ASSERT_TRUE(is_path_allowed("", t.root.string(), {"*"}));
    ASSERT_FALSE(is_path_allowed("", t.root.string(), {"/srv/backup/*"}));
}

/* Список разрешённых снаружи сверяется с тем, что написал человек, а не с
 * развёрнутыми симлинками: иначе «всегда на /srv/backup» перестало бы
 * работать, стоит только появиться симлинку. */
TEST(path_guard_allowed_list_is_matched_against_the_written_path) {
    Tree t = make_tree();
    const std::string root = t.root.string();
    /* Свой путь — разрешён без списка: он внутри проекта. */
    ASSERT_TRUE(is_path_allowed((t.root / "a.php").string(), root, {}));
    /* Ровно тот же путь, но человек разрешил этот каталог префиксом. */
    ASSERT_TRUE(is_path_allowed(t.escape, root, {root + "/escape/*"}));
    /* Точное совпадение — тоже разрешение: список набирают и точечно. */
    ASSERT_TRUE(is_path_allowed(t.escape, root, {t.escape}));
    /* Соседний каталог с тем же началом строки — не разрешён: префикс
     * ровно такой, а `*` без ведущего слэша не спасает. */
    ASSERT_FALSE(is_path_allowed(t.escape, root,
                                 {root + "/escape"}));
    ASSERT_FALSE(is_path_allowed(t.escape, root,
                                 {root + "/escap"}));
    /* Пустой список — не разрешение ничему. */
    ASSERT_FALSE(is_path_allowed(t.escape, root, {""}));
}

/* ======================================================================
 * 2. Потребители: гейт разрешений и инструмент записи
 * ====================================================================== */

/* Гейт — единственное место, где «вне проекта» превращается в вопрос к
 * человеку. Если правка решения живёт в функции, но гейт смотрит в другое
 * место, механизм есть, а механизм не работает. */
TEST(permission_gate_treats_symlink_escape_as_external) {
    Tree t = make_tree();
    ProjectFixture fx(t.root);

    EngineState st;
    HostCallbacks cb;
    PermissionGate gate(st, cb, [](AgentEvent::Kind, const std::string&) {});
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        st.project_dir = t.root.string();
        st.state = AgentState::Executing;
    }
    /* Файл проекта вопроса не вызывает — иначе проверка на симлинк была бы
     * зелёной и при отказе гейта вообще. */
    ASSERT_TRUE(gate.check((t.root / "a.php").string()).empty());
    ASSERT_TRUE(gate.check((t.root / "wp-content" / "themes" / "style.css")
                               .string())
                    .empty());
    /* Уход по симлинку — вопрос (здесь запрет правилом, и это видно по
     * тексту: путь назван, а не подставлен). */
    const std::string refusal = gate.check(t.escape);
    ASSERT_TRUE(!refusal.empty());
    ASSERT_TRUE(refusal.find(t.escape) != std::string::npos);
}

/* Инструмент `write_file` — тот путь, ради которого всё и затевалось:
 * запись ПО СИМЛИНКУ наружу не должна создавать файл снаружи проекта.
 * Проверяется не текст ответа, а ФАКТ: файла снаружи нет. Текст ответа
 * можно переформулировать, факт — нет.
 *
 * Положительный контроль пишет в файл, который уже ЕСТЬ, и сверяет
 * содержимое, а не существование: файл из фикстуры существует и до
 * вызова инструмента, и «файл на месте» было бы верно при отказе записи
 * вообще. Путь с новым каталогом для контроля не годится по отдельной
 * причине: `write_file` не создаёт родительские каталоги (обычный
 * `ofstream`), и контроль падал бы не на guard'е, а на этом — то есть
 * читался бы как дефект правки, которой нет. */
TEST(write_file_through_symlink_escape_writes_nothing_outside) {
    Tree t = make_tree();
    ProjectFixture fx(t.root);

    const std::string stolen = t.escape;
    const std::string result = run_write("escape/stolen.php", "<?php evil\n");
    ASSERT_TRUE(!exists(stolen));
    ASSERT_TRUE(result.find("запрещён") != std::string::npos ||
                result.find("вне проекта") != std::string::npos);

    const std::string theme = (t.root / "wp-content" / "themes" / "style.css")
                                  .string();
    const std::string ok = run_write("wp-content/themes/style.css",
                                     "body{color:red}\n");
    ASSERT_EQ(read_file(fs::path(theme)), std::string("body{color:red}\n"));
    ASSERT_TRUE(ok.find("записано") != std::string::npos);
}

/* Отказ на симлинке не должен ломать соседние пути: проект с симлинком
 * остаётся рабочим целиком. Без этой проверки исправление выглядело бы
 * «запретить всё» — и прошло бы все проверки выше. Сверяется содержимое,
 * а не существование: файлы созданы фикстурой и существовали бы в любом
 * случае. */
TEST(project_with_a_symlink_keeps_writing_ordinary_files) {
    Tree t = make_tree();
    ProjectFixture fx(t.root);

    run_write("a.php", "<?php one\n");
    run_write("wp-content/themes/style.css", "body{color:blue}\n");
    ASSERT_EQ(read_file(t.root / "a.php"), std::string("<?php one\n"));
    ASSERT_EQ(read_file(t.root / "wp-content" / "themes" / "style.css"),
              std::string("body{color:blue}\n"));
}