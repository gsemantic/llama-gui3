#include "python_tools.h"
#include "../../core/tool.h"
#include "../../core/tools_registry.h"
#include "../../core/engine.h"
#include "../../core/project.h"
#include "../../core/shell.h"
#include "../../core/security.h"
#include "../../core/limits.h"

#include <cstdio>
#include <sstream>
#include <filesystem>
#include <functional>
#include <iostream>

namespace fs = std::filesystem;
namespace coder {
namespace python {

namespace {

using limits::kModuleMaxOutput;

/* И1.3: чтение типизированных аргументов. */
std::string arg_str(const json::JsonValue& a, const char* key) {
    return a.get_string(key);
}

ToolOutput out(std::string title, std::string text) {
    ToolOutput o;
    o.title = std::move(title);
    o.output = std::move(text);
    return o;
}

std::string python_run(const std::string& path) {
    std::string abs = project_resolve(path);
    std::string cmd = "python3 " + shell::shell_quote(abs);
    std::string result = shell::run_capture(cmd, 60);
    return result.empty() ? "[python_run: нет вывода]" : shell::cap(result, kModuleMaxOutput);
}

/* pip виртуального окружения проекта, если оно есть.
 *
 * `.venv` проверяется раньше `venv`: это имя создаёт `python3 -m venv
 * .venv`, то есть дефолт. Симлинк pip — обычное дело, поэтому exists, а не
 * is_regular_file. На Windows у venv нет bin/, поэтому путь другой; модуль
 * собирается и там, и разница пути не должна превращаться в «venv не
 * найден». */
std::string project_venv_pip(const std::string& project_dir) {
    if (project_dir.empty()) return "";
    for (const char* name : {".venv", "venv"}) {
        const fs::path base = fs::path(project_dir) / name;
        const fs::path posix = base / "bin" / "pip";
        std::error_code ec;
        if (fs::exists(posix, ec)) return posix.string();
#ifdef _WIN32
        const fs::path win = base / "Scripts" / "pip.exe";
        if (fs::exists(win, ec)) return win.string();
#endif
    }
    return "";
}

std::string pip_install(const std::string& pkg,
                        const std::string& project_dir,
                        const std::function<bool()>& should_cancel) {
    if (pkg.empty()) return "[ошибка] укажи имя пакета (query)";
    /* Куда ставить пакет.
     *
     * Раньше здесь был безусловный `pip install <pkg>`. На Debian/Ubuntu с
     * PEP 668 такой вызов всегда падает с `externally-managed-environment`
     * — то есть инструмент не работал на целевой системе вообще, а модель
     * читала сырой traceback и делала вывод «в этой среде ставить нельзя».
     * Порядок выбора:
     *   1. venv проекта (.venv или venv) — правильное место для
     *      зависимостей, и PEP 668 к venv не применяется;
     *   2. системный pip с `--user --break-system-packages` — пакет
     *      ложится в ~/.local, root не нужен, системный Python не ломается;
     *   3. если pip старый и не знает флаг — понятная инструкция, а не
     *      «no such option». */
    const std::string venv_pip = project_venv_pip(project_dir);
    const bool use_venv = !venv_pip.empty();
    std::string cmd;
    if (use_venv) {
        cmd = shell::shell_quote(venv_pip) + " install " + shell::shell_quote(pkg);
    } else {
        cmd = "pip install --user --break-system-packages " +
              shell::shell_quote(pkg);
    }
    /* И11.10: установка пакета идёт долго и обрывается на полпути, если её
     * не остановить; без отметки об отмене модель сочтёт установку
     * успешной. */
    shell::RunStats stats;
    std::string result =
        shell::run_capture_cancel(cmd, 120, should_cancel, stats);
    if (stats.cancelled) result += shell::cancelled_note();

    /* PEP 668 остаётся возможным даже с --user на некоторых сборках.
     * Возвращаем модели действие, а не сырой traceback: иначе она
     * повторяет тот же вызов и закрывает задачу словами «среда не
     * позволяет». */
    if (result.find("externally-managed-environment") != std::string::npos) {
        return "[pip] Установка в системный Python заблокирована (PEP 668).\n"
               "Создай venv в проекте: python3 -m venv .venv — и повтори "
               "вызов: pip_install сам использует .venv/bin/pip.\n"
               "Для системного пакета: apt install python3-<имя>.";
    }
    if (!use_venv &&
        result.find("break-system-packages") != std::string::npos &&
        result.find("no such option") != std::string::npos) {
        return "[pip] Этот pip не поддерживает --break-system-packages.\n"
               "Создай venv в проекте: python3 -m venv .venv — и повтори "
               "вызов.";
    }
    return result.empty() ? "[pip install: нет вывода]"
                          : shell::cap(result, kModuleMaxOutput);
}

std::string django_manage(const std::string& args, const std::string& project_dir) {
    std::string manage = project_dir + "/manage.py";
    if (!fs::exists(manage)) return "[ошибка] manage.py не найден в " + project_dir;
    /* И13.4 (D9): args — текст от модели, и раньше он уходил в строку
     * команды как есть. Это не «аргументы без кавычек» (так D9 был
     * записан), а выполнение произвольной команды: «migrate; touch …»
     * разбирается оболочкой на две команды, и вторая выполняется.
     * Проверено на живой оболочке с настоящим manage.py.
     *
     * Экранирование здесь не помогло бы: manage.py получает АРГУМЕНТЫ, а
     * не строку shell, и кавычки в аргументах оболочка разбирает сама
     * («test "my tests.py"» доходит тремя словами). Второй слой
     * экранирования только испортил бы разбор. Поэтому запрещён ровно
     * разделитель — перевод строки им тоже является, — а всё остальное
     * доходит до manage.py как есть.
     *
     * Отказ называет символ: модель должна знать, что именно не так, иначе
     * отказ читается как поломка инструмента и повторяется. */
    const std::string why = security::shell_arg_refusal(args);
    if (!why.empty())
        return std::string("[запрещено] в аргументах manage.py: ") + why +
               ". Один вызов — одна manage-команда: «migrate»,"
               " «test tests/test_x.py», «--settings=…». "
               "Если нужна цепочка — это отдельный вызов инструмента.";
    std::string cmd = "python3 " + shell::shell_quote(manage) + " " + args;
    std::string result = shell::run_capture(cmd, 60);
    return result.empty() ? "[django: нет вывода]" : shell::cap(result, kModuleMaxOutput);
}

std::string pytest_run(const std::string& path, const std::string& marker,
                       const std::function<bool()>& should_cancel) {
    std::string cmd = "python3 -m pytest";
    if (!path.empty()) cmd += " " + shell::shell_quote(project_resolve(path));
    if (!marker.empty()) cmd += " -m " + shell::shell_quote(marker);
    cmd += " -v";
    /* И11.10: прогон тестов — самый частый долгий вызов, и самый частый
     * повод нажать «стоп». */
    shell::RunStats stats;
    std::string result =
        shell::run_capture_cancel(cmd, 120, should_cancel, stats);
    if (stats.cancelled) result += shell::cancelled_note();
    return result.empty() ? "[pytest: нет вывода]" : shell::cap(result, kModuleMaxOutput);
}

std::string venv_create(const std::string& path) {
    std::string abs = project_resolve(path);
    std::string cmd = "python3 -m venv " + shell::shell_quote(abs);
    std::string result = shell::run_capture(cmd, 60);
    return result.empty() ? "[venv: создано]" : shell::cap(result, kModuleMaxOutput);
}

std::string python_lint(const std::string& path) {
    std::string abs = project_resolve(path);
    std::string cmd = "python3 -m py_compile " + shell::shell_quote(abs);
    std::string result = shell::run_capture(cmd, 30);
    return result.empty() ? "[py_compile: нет ошибок]" : shell::cap(result, kModuleMaxOutput);
}

} // anonymous namespace

void register_python_tools() {
    auto& reg = ToolsRegistry::instance();

    {
        ToolDef def;
        def.name = "python_run";
        def.description = "Запуск Python-скрипта";
        /* Скрипт проекта может писать файлы и ходить в сеть — эффект
         * заранее неизвестен, поэтому считаем изменением. */
        def.flags = TF_EXECUTES | TF_DESTRUCTIVE;
        def.permission_key = "bash";
        SchemaBuilder b;
        b.str("path", "путь к скрипту").required("path");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            return out("python", python_run(arg_str(a, "path")));
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "pip_install";
        def.description =
            "Установка Python-пакета. Если в проекте есть .venv/venv — "
            "ставит в него; иначе в ~/.local (--user --break-system-packages).";
        /* Меняет окружение, а не файлы проекта: откатить нечем. */
        def.flags = TF_EXECUTES | TF_DESTRUCTIVE | TF_SLOW | TF_NETWORK;
        def.permission_key = "package";
        SchemaBuilder b;
        b.str("query", "имя пакета (например requests)").required("query");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            return out("pip", pip_install(arg_str(a, "query"), ctx.project_dir(),
                                          [&ctx]() { return ctx.cancelled(); }));
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "django_manage";
        def.description = "Django management-команда (manage.py)";
        /* «migrate», «flush», «createsuperuser» меняют БД. */
        def.flags = TF_EXECUTES | TF_DESTRUCTIVE | TF_SLOW;
        def.permission_key = "bash";
        SchemaBuilder b;
        b.str("cli", "аргументы manage.py, например «migrate» или «test»")
         .required("cli");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            return out("django", django_manage(arg_str(a, "cli"), ctx.project_dir()));
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "pytest_run";
        def.description = "Запуск тестов pytest";
        /* Фикстуры и сами тесты пишут файлы и кэш. */
        def.flags = TF_EXECUTES | TF_WRITES_FILES | TF_SLOW;
        def.permission_key = "test";
        SchemaBuilder b;
        b.str("path", "файл или каталог с тестами; пусто = весь проект")
         .str("query", "маркер pytest, например «unit»");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext& ctx) -> ToolOutput {
            return out("pytest", pytest_run(arg_str(a, "path"), arg_str(a, "query"),
                                            [&ctx]() { return ctx.cancelled(); }));
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "venv_create";
        def.description = "Создание виртуального окружения";
        def.flags = TF_EXECUTES | TF_WRITES_FILES | TF_SLOW;
        def.permission_key = "package";
        SchemaBuilder b;
        b.str("path", "каталог окружения, например venv").required("path");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            return out("venv", venv_create(arg_str(a, "path")));
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "python_lint";
        def.description = "Проверка синтаксиса Python (py_compile)";
        def.flags = TF_READ_ONLY | TF_EXECUTES;
        def.permission_key = "read";
        SchemaBuilder b;
        b.str("path", "путь к файлу .py").required("path");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            return out("py_compile", python_lint(arg_str(a, "path")));
        };
        reg.register_def(std::move(def));
    }
}

static const char* kDjangoSkill =
    "# python_django\n"
    "Описание: Django best practices\n"
    "- Проект: django-admin startproject <name>.\n"
    "- Приложение: python manage.py startapp <name>.\n"
    "- Модели: class MyModel(models.Model), Meta: verbose_name.\n"
    "- Views: Function-based (def) или Class-based (View, ListView, DetailView).\n"
    "- URLs: path('route/', view, name='name').\n"
    "- Шаблоны: {% extends 'base.html' %}, {% block content %}.\n"
    "- Миграции: python manage.py makemigrations && migrate.\n"
    "- Админка: @admin.register(MyModel) в admin.py.\n"
    "- Тесты: django.test.TestCase, client = Client().";

static const char* kFlaskSkill =
    "# python_flask\n"
    "Описание: Flask best practices\n"
    "- Создание: app = Flask(__name__).\n"
    "- Роутинг: @app.route('/path', methods=['GET', 'POST']).\n"
    "- Шаблоны: render_template('template.html', **kwargs).\n"
    "- БД: Flask-SQLAlchemy (db = SQLAlchemy(app)).\n"
    "- Формы: Flask-WTF (wtforms).\n"
    "- Конфиг: app.config.from_object(Config).\n"
    "- Тесты: pytest + app.test_client().";

static const char* kFastapiSkill =
    "# python_fastapi\n"
    "Описание: FastAPI best practices\n"
    "- Создание: app = FastAPI().\n"
    "- Роутинг: @app.get('/path'), @app.post('/path').\n"
    "- Pydantic: модели для request/response (class Item(BaseModel)).\n"
    "- Зависимости: Depends(get_db).\n"
    "- Async: async def endpoint().\n"
    "- Docs: /docs (Swagger), /redoc (ReDoc).";

static const char* kPythonProjectSkill =
    "# python_project\n"
    "Описание: структура Python-проекта\n"
    "- pyproject.toml или setup.py + requirements.txt.\n"
    "- Виртуальное окружение: python -m venv venv && source venv/bin/activate.\n"
    "- Структура: src/ или прямое размещение.\n"
    "- Тесты: tests/ с pytest.\n"
    "- Lint: ruff check / flake8 / black --check.\n"
    "- Типизация: mypy или pyright.";

std::vector<Skill> get_python_skills() {
    return {
        {"python_django", "Django best practices", kDjangoSkill, "python"},
        {"python_flask", "Flask best practices", kFlaskSkill, "python"},
        {"python_fastapi", "FastAPI best practices", kFastapiSkill, "python"},
        {"python_project", "Структура Python-проекта", kPythonProjectSkill, "python"}
    };
}

} // namespace python
} // namespace coder
