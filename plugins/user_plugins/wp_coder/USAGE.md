# wp_coder — Использование

## Быстрый старт

1. **Запуск с агентом ai_coder:**
   ```bash
   env -u LD_PRELOAD ./build/llama-gui-core --agent=ai_coder
   ```
   Флаг `--agent=ai_coder` переключает чат приложения на плагинный ReAct-агент.
   Если локальный LLM-сервер ещё не запущен — поднимите его (например `llama-server`).

2. **Настройка проекта** — меню **AI Coder → Проект** (`Ctrl+Shift+W`):
   - Корень проекта (например `/var/www/html`)
   - URL локального сайта для `verify`/`headless_render`
   - Путь к php-cli (обычно автоопределяется)
   - Нажмите «Сохранить все настройки»

3. **Выбор модуля** — меню **AI Coder → Модули** (`Ctrl+Shift+M`):
   - WordPress / Python / DevOps
   - От выбора зависят доступные инструменты и навыки в системном промпте

4. **Отправка задачи** — введите текст в поле чата и нажмите Enter.
   Прогресс виден в реальном времени (шаги агента форвардятся в чат через `chat_event`).
   Метрики шага: `%d tok | %.1f tok/s | %ds (LLM %.1fs) | %d steps`.
   Кнопка «Стоп» прерывает текущую задачу.

5. **Разрешения** — при доступе к файлам вне проекта появится запрос:
   «Разрешить (один раз)» / «Разрешить (всегда)» / «Отклонить».

## Инструменты по группам

### Базовые (core)
- `read_file` / `write_file` / `search_replace` — работа с файлами (с капом вывода 12 Кб)
- `grep_search` — regex-поиск (лимит 200 совпадений)
- `repo_map` — компактный обзор структуры проекта (кэшируется на задачу)
- `exec_command` — выполнение команды (timeout 60, проверка blocked-команд)
- `list_skills` / `skill_detail` — ленивая загрузка тел навыков
- `rag_index` / `rag_query` — индексация и поиск по документам

### Git
- `git_status`, `git_diff`, `git_log`, `git_commit`

### WordPress
- `wp_cli`, `wp_db`, `wp_media`, `wp_option`, `wp_rest`, `wp_create_site`,
  `wp_check_deps`, `deploy`, `verify`, `php_lint`, `headless_render`, `validate`

### Python
- `python_run`, `pip_install`, `django_manage`, `pytest_run`, `venv_create`, `python_lint`

### DevOps
- `docker_build`, `docker_run`, `docker_ps`, `docker_logs`,
  `systemd_status`, `systemd_restart`, `nginx_test`, `nginx_reload`,
  `cron_list`, `cron_add`, `ssh_exec`

## Навыки

Inline-навыки модулей (тела не в промпте — подгружаются через `skill_detail`):

| Модуль | Навыки |
|--------|--------|
| WordPress | `wp_theme`, `wp_hook`, `wp_database`, `wp_media`, `wp_plugin_boilerplate`, `wp_git` |
| Python | `python_django`, `python_flask`, `python_fastapi`, `python_project` |
| DevOps | `devops_docker`, `devops_systemd`, `devops_nginx` |

Внешний навык из `skills/`: `wp_setup.md` (настройка окружения WordPress).

### Свои навыки
Положите `.md` в каталог данных: `<data_dir>/wp_coder/skills/my_skill.md`.
Формат: первая строка `# Имя`, вторая — описание, далее — тело инструкции.
При совпадении имени с inline-навыком модуля inline имеет приоритет.

## Режимы

- **Code** — полный доступ ко всем инструментам
- **Research** — только чтение (нет `write_file` и `deploy`)
- **Review** — после правок автоматически запускает `verify`

## Окна

| Окно | Горячая клавиша |
|------|-----------------|
| Проект | `Ctrl+Shift+W` |
| Модули | `Ctrl+Shift+M` |
| Инструменты | `Ctrl+Shift+T` |

В окне «Инструменты» показан список зарегистрированных инструментов выбранного модуля.
