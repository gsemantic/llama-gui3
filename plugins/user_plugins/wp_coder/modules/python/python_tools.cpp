#include "python_tools.h"
#include "../../core/tools_registry.h"
#include "../../core/engine.h"
#include "../../core/shell.h"

#include <cstdio>
#include <sstream>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;
namespace coder {
namespace python {

namespace {

constexpr size_t kPyMaxOutput = 8000;

std::string resolve_path(const std::string& rel) {
    const auto& st = engine_state();
    if (st.project_dir.empty()) return rel;
    if (rel.empty()) return st.project_dir;
    if (rel[0] == '/') return rel;
    return st.project_dir + "/" + rel;
}

std::string python_run(const std::string& path) {
    std::string abs = resolve_path(path);
    std::string cmd = "python3 " + shell::shell_quote(abs);
    std::string out = shell::run_capture(cmd, 60);
    return out.empty() ? "[python_run: нет вывода]" : shell::cap(out, kPyMaxOutput);
}

std::string pip_install(const std::string& pkg) {
    if (pkg.empty()) return "[ошибка] укажи имя пакета (QUERY)";
    std::string cmd = "pip install " + shell::shell_quote(pkg);
    std::string out = shell::run_capture(cmd, 120);
    return out.empty() ? "[pip install: нет вывода]" : shell::cap(out, kPyMaxOutput);
}

std::string django_manage(const std::string& args) {
    const auto& st = engine_state();
    std::string manage = st.project_dir + "/manage.py";
    if (!fs::exists(manage)) return "[ошибка] manage.py не найден в " + st.project_dir;
    std::string cmd = "python3 " + shell::shell_quote(manage) + " " + args;
    std::string out = shell::run_capture(cmd, 60);
    return out.empty() ? "[django: нет вывода]" : shell::cap(out, kPyMaxOutput);
}

std::string pytest_run(const std::string& path, const std::string& marker) {
    std::string cmd = "python3 -m pytest";
    if (!path.empty()) cmd += " " + shell::shell_quote(resolve_path(path));
    if (!marker.empty()) cmd += " -m " + shell::shell_quote(marker);
    cmd += " -v";
    std::string out = shell::run_capture(cmd, 120);
    return out.empty() ? "[pytest: нет вывода]" : shell::cap(out, kPyMaxOutput);
}

std::string venv_create(const std::string& path) {
    std::string abs = resolve_path(path);
    std::string cmd = "python3 -m venv " + shell::shell_quote(abs);
    std::string out = shell::run_capture(cmd, 60);
    return out.empty() ? "[venv: создано]" : shell::cap(out, kPyMaxOutput);
}

std::string python_lint(const std::string& path) {
    std::string abs = resolve_path(path);
    std::string cmd = "python3 -m py_compile " + shell::shell_quote(abs);
    std::string out = shell::run_capture(cmd, 30);
    return out.empty() ? "[py_compile: нет ошибок]" : shell::cap(out, kPyMaxOutput);
}

} // anonymous namespace

void register_python_tools() {
    auto& reg = ToolsRegistry::instance();

    reg.register_tool("python_run", [](const ToolArgs& a) -> std::string {
        return python_run(a.path);
    }, "Запуск Python-скрипта");

    reg.register_tool("pip_install", [](const ToolArgs& a) -> std::string {
        return pip_install(a.query);
    }, "Установка пакета");

    reg.register_tool("django_manage", [](const ToolArgs& a) -> std::string {
        return django_manage(a.cli);
    }, "Django management команда");

    reg.register_tool("pytest_run", [](const ToolArgs& a) -> std::string {
        return pytest_run(a.path, a.query);
    }, "Запуск тестов");

    reg.register_tool("venv_create", [](const ToolArgs& a) -> std::string {
        return venv_create(a.path);
    }, "Создание виртуального окружения");

    reg.register_tool("python_lint", [](const ToolArgs& a) -> std::string {
        return python_lint(a.path);
    }, "Проверка синтаксиса Python");
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
