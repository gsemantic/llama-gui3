/*
 * test_python_tools.cpp — безопасность и валидация Python-инструментов (4.4).
 *
 * Тестируем ветки валидации ДО запуска команд (pip_install с пустым именем).
 * Функции, выполняющие команды (python_run, pytest_run и т.д.), не вызываются —
 * они тестируются только интеграционно, вручную.
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/json.h"
#include "../core/tools_registry.h"
#include "../core/engine.h"
#include "../modules/python/python_tools.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unistd.h>

using namespace coder;
namespace fs = std::filesystem;

static void init_engine_and_python() {
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&, LlmReply&) { return false; };
    cb.llm_complete = [](const std::string&, const std::string&, std::string&) { return false; };
    cb.llm_is_connected = []() { return false; };
    cb.chat_event = [](const std::string&) {};
    Engine::instance().init(cb);
    test_support::approve_all_permissions();
    python::register_python_tools();
}

TEST(pip_install_empty_rejected) {
    init_engine_and_python();
    /* И1.6: query обязателен по схеме — пустой вызов отсекается валидацией. */
    ToolArgs a;  // query пуст
    std::string r = ToolsRegistry::instance().run("pip_install", a);
    ASSERT_TRUE(r.find("invalid arguments") != std::string::npos);
    ASSERT_TRUE(r.find("query") != std::string::npos);
    /* Явно переданный пустой query доходит до обработчика. */
    json::JsonValue args = json::JsonValue::object();
    args.set("query", "");
    std::string r2 = ToolsRegistry::instance().run("pip_install", args);
    ASSERT_TRUE(r2.find("укажи имя пакета") != std::string::npos);
}

TEST(python_tools_registered) {
    init_engine_and_python();
    auto& reg = ToolsRegistry::instance();
    ASSERT_TRUE(reg.has("python_run"));
    ASSERT_TRUE(reg.has("pip_install"));
    ASSERT_TRUE(reg.has("pytest_run"));
    ASSERT_TRUE(reg.has("venv_create"));
    ASSERT_TRUE(reg.has("python_lint"));
    ASSERT_TRUE(reg.has("django_manage"));
}

/* ======================================================================
 * И13.4 (D9): аргументы manage.py не превращаются в команду
 *
 * Проверка поведенческая, а не «функция вернула отказ»: материалом
 * служит настоящий проект с настоящим manage.py, который при запуске
 * оставляет файл-след. Поэтому «инъекция не выполнилась» и «manage.py
 * вообще не запускался» — разные исходы, и положительный контроль ниже
 * показывает, что фикстура запускается и след пишется.
 * ====================================================================== */

namespace {

/* Каталог проекта с manage.py, который пишет файл-след: так проверка
 * видит, что команда РЕАЛЬНО выполнялась, а не что строка собралась. */
fs::path django_project(const std::string& tag, const fs::path& marker) {
    fs::path dir = fs::temp_directory_path()
        / ("wp_coder_django_" + std::to_string(::getpid()) + "_" +
           std::to_string(std::rand()) + "_" + tag);
    fs::remove_all(dir);
    fs::create_directories(dir);
    register_tmp_tree(dir.string());
    std::ofstream m(dir / "manage.py", std::ios::binary);
    m << "import sys\n"
      << "open(" << '"' << marker.string() << '"' << ", 'w').write(' '.join(sys.argv[1:]))\n"
      << "print('manage.py выполнен:', sys.argv[1:])\n";
    m.close();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = dir.string();
    }
    return dir;
}

std::string run_django(const std::string& cli) {
    json::JsonValue a = json::JsonValue::object();
    a.set("cli", cli);
    return ToolsRegistry::instance().run("django_manage", a);
}

} // namespace

TEST(django_manage_runs_a_plain_command) {
    init_engine_and_python();
    const fs::path marker = fs::temp_directory_path() /
        ("wp_coder_django_mark_" + std::to_string(::getpid()) + "_run");
    fs::remove(marker);
    django_project("run", marker);

    const std::string out = run_django("migrate --noinput");
    /* Положительный контроль на ТО ЖЕ, что и проверка ниже: фикстура
     * обязана запускаться, иначе «инъекция не выполнилась» ничего не
     * значит. */
    ASSERT_TRUE(out.find("запрещено") == std::string::npos);
    ASSERT_TRUE(fs::exists(marker));
    std::ifstream f(marker, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    ASSERT_EQ(ss.str(), std::string("migrate --noinput"));
    std::error_code ec;
    fs::remove(marker, ec);
}

TEST(django_manage_refuses_arguments_that_start_a_second_command) {
    init_engine_and_python();
    const fs::path marker = fs::temp_directory_path() /
        ("wp_coder_django_mark_" + std::to_string(::getpid()) + "_inj");
    fs::remove(marker);
    django_project("inj", marker);

    /* Материал — то, что разбирается оболочкой на ДВЕ команды. Проверено
     * на живой оболочке с этим же manage.py: вторая команда выполнялась
     * и оставляла файл. Кавычка в материале есть потому, что «ломает
     * выход» и «выполняет команду» — разные дефекты, и закрыть надо
     * второй. */
    const char* attacks[] = {
        "migrate; touch MARK",
        "migrate | touch MARK",
        "migrate & touch MARK",
        "migrate\ntouch MARK",
        "migrate && touch MARK",
        "migrate `touch MARK`",
        "migrate $(touch MARK)",
        "migrate > MARK",
    };
    for (const char* raw : attacks) {
        std::string attack = raw;
        for (size_t at = attack.find("MARK"); at != std::string::npos;
             at = attack.find("MARK", at + 1)) {
            attack.replace(at, 4, marker.string());
        }
        const std::string out = run_django(attack);
        const bool injected = fs::exists(marker);
        if (injected || out.find("запрещено") == std::string::npos) {
            std::cerr << "  «" << attack << "» → след есть: "
                      << (injected ? "да" : "нет")
                      << ", отказ в ответе: "
                      << (out.find("запрещено") != std::string::npos ? "да" : "нет")
                      << std::endl;
        }
        ASSERT_FALSE(injected);
        ASSERT_TRUE(out.find("запрещено") != std::string::npos);
        std::error_code ec;
        fs::remove(marker, ec);
    }
}

TEST(django_manage_still_passes_arguments_that_only_need_quoting) {
    /* Защита не должна запрещать легальный вызов: кавычки и пробелы —
     * обычное дело для Django («test "my tests.py"»). Если этот тест
     * красный, запрет бьёт по работе, и модель будет повторять отказ. */
    init_engine_and_python();
    const fs::path marker = fs::temp_directory_path() /
        ("wp_coder_django_mark_" + std::to_string(::getpid()) + "_ok");
    fs::remove(marker);
    django_project("ok", marker);

    const std::string out = run_django("test \"my tests.py\" --settings=settings.prod");
    ASSERT_TRUE(out.find("запрещено") == std::string::npos);
    ASSERT_TRUE(fs::exists(marker));
    std::ifstream f(marker, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    /* manage.py получил ТРИ аргумента, а не один склеенный: разбор
     * остался башевским, экранирование не понадобилось. */
    ASSERT_EQ(ss.str(), std::string("test my tests.py --settings=settings.prod"));
    std::error_code ec;
    fs::remove(marker, ec);
}