#include "python_tools.h"
#include "../../core/tool.h"
#include "../../core/tools_registry.h"
#include "../../core/engine.h"
#include "../../core/project.h"
#include "../../core/shell.h"
#include "../../core/limits.h"

#include <cstdio>
#include <sstream>
#include <filesystem>
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

std::string pip_install(const std::string& pkg) {
    if (pkg.empty()) return "[ошибка] укажи имя пакета (query)";
    std::string cmd = "pip install " + shell::shell_quote(pkg);
    std::string result = shell::run_capture(cmd, 120);
    return result.empty() ? "[pip install: нет вывода]" : shell::cap(result, kModuleMaxOutput);
}

std::string django_manage(const std::string& args, const std::string& project_dir) {
    std::string manage = project_dir + "/manage.py";
    if (!fs::exists(manage)) return "[ошибка] manage.py не найден в " + project_dir;
    std::string cmd = "python3 " + shell::shell_quote(manage) + " " + args;
    std::string result = shell::run_capture(cmd, 60);
    return result.empty() ? "[django: нет вывода]" : shell::cap(result, kModuleMaxOutput);
}

std::string pytest_run(const std::string& path, const std::string& marker) {
    std::string cmd = "python3 -m pytest";
    if (!path.empty()) cmd += " " + shell::shell_quote(project_resolve(path));
    if (!marker.empty()) cmd += " -m " + shell::shell_quote(marker);
    cmd += " -v";
    std::string result = shell::run_capture(cmd, 120);
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
        def.description = "Установка Python-пакета";
        /* Меняет окружение, а не файлы проекта: откатить нечем. */
        def.flags = TF_EXECUTES | TF_DESTRUCTIVE | TF_SLOW | TF_NETWORK;
        def.permission_key = "package";
        SchemaBuilder b;
        b.str("query", "имя пакета (например requests)").required("query");
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            return out("pip", pip_install(arg_str(a, "query")));
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
        def.handler = [](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            return out("pytest", pytest_run(arg_str(a, "path"), arg_str(a, "query")));
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
