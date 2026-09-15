# wp_coder — Использование

## Быстрый старт

1. **Запуск с агентом ai_coder:**
   ```bash
   env -u LD_PRELOAD ./build/llama-gui-core --agent=ai_coder
   ```

2. **Настройка проекта** — меню **AI Coder → Проект** (`Ctrl+Shift+W`):
   - Корень проекта (например `/var/www/html`)
   - URL локального сайта для `verify`/`headless_render`
   - Путь к php-cli (обычно автоопределяется)
   - Нажмите «Сохранить все настройки»

3. **Выбор модуля** — меню **AI Coder → Модули** (`Ctrl+Shift+M`):
   - WordPress / Python / DevOps
   - От выбора зависят доступные инструменты и навыки

4. **Отправка задачи** — введите текст в поле чата и нажмите Enter.
   Метрики шага: `%d tok | %.1f tok/s | %ds (LLM %.1fs) | %d steps`.
   Кнопка «Стоп» прерывает текущую задачу.

5. **Разрешения** — при доступе к файлам вне проекта появится запрос:
   «Разрешить (один раз)» / «Разрешить (всегда)» / «Отклонить».

## Режимы

- **Code** — полный доступ ко всем инструментам
- **Research** — только чтение (нет `write_file` и `deploy`)
- **Review** — после правок автоматически запускает `verify`

Переключение: выпадающий список «Режим» в области под полем ввода чата.

## Опции агента

| Опция | Описание | По умолчанию |
|-------|----------|--------------|
| Продолжать сессию | План и контекст сохраняются между сообщениями | Вкл |
| План-режим | Правки не применяются, а предлагаются | Выкл |
| Очистить сессию | Сбросить план и историю диалога | — |

## Окна

| Окно | Горячая клавиша | Содержимое |
|------|-----------------|------------|
| Проект | `Ctrl+Shift+W` | Настройки: корень, PHP, WP-URL, деплой, промпт |
| Модули | `Ctrl+Shift+M` | Выбор модуля, счётчики инструментов/навыков |
| Инструменты | `Ctrl+Shift+T` | Список зарегистрированных инструментов, активные навыки |

## Список инструментов

### Базовые (core)
- `read_file` / `write_file` / `search_replace` / `edit_file` / `undo_edit` — работа с файлами
- `grep_search` — regex-поиск (лимит 200 совпадений)
- `repo_map` — обзор структуры проекта (кэшируется)
- `list_dir` — лёгкий список файлов/каталогов
- `web_fetch` — HTTP GET запрос через curl
- `exec_command` — выполнение команды (timeout 60s, проверка blocked-команд)
- `list_skills` / `skill_detail` — ленивая загрузка тел навыков
- `rag_index` / `rag_query` — индексация и поиск по документам

### Git
- `git_status`, `git_diff`, `git_log`
- `git_add` (частичная индексация), `git_branch`, `git_checkout`
- `git_commit` (по всем или только по PATH)

### WordPress
- `wp_cli`, `wp_db`, `wp_media`, `wp_option`, `wp_rest`
- `wp_create_site`, `wp_check_deps`, `deploy`, `verify`
- `php_lint`, `headless_render`, `validate`

### Python
- `python_run`, `pip_install`, `django_manage`
- `pytest_run`, `venv_create`, `python_lint`

### DevOps
- `docker_build`, `docker_run`, `docker_ps`, `docker_logs`
- `systemd_status`, `systemd_restart`
- `nginx_test`, `nginx_reload`
- `cron_list`, `cron_add`, `ssh_exec`

## Свои навыки

Положите `.md` в каталог данных: `<data_dir>/wp_coder/skills/my_skill.md`.
Формат: первая строка `# Имя`, вторая — описание, далее — тело инструкции.
При совпадении имени с inline-навыком модуля inline имеет приоритет.
