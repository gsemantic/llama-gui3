# wp_coder — AI-кодер с модульной архитектурой

Плагин `wp_coder` для llama-gui — универсальный ReAct-агент с доменными модулями.
WordPress — один из модулей; Python и DevOps подключаются аналогично.

- **Версия:** 0.3.0 (см. `plugin.json`)
- **Agent mode:** `ai_coder` (отображается как «AI Coder» в чате приложения)

## Архитектура

```
wp_coder/
├── core/                 # Универсальное ядро (не зависит от доменов)
│   ├── engine.{h,cpp}            # ReAct-движок: multi-turn сессия, метрики, LLM-callbacks
│   ├── agent_components.{h,cpp}  # SessionStore, PermissionGate, ToolRunner, Planner, AgentLoop
│   ├── tools_registry.{h,cpp}    # Динамический реестр инструментов
│   ├── skills_manager.{h,cpp}    # Навыки: inline из модулей + .md из skills/
│   ├── tool_protocol.{h,cpp}     # Парсер wp_action / JSON tool_calls
│   ├── base_tools.{h,cpp}        # read/write/search_replace/repo_map/grep/exec/rag
│   ├── git_tools.{h,cpp}         # git_status/diff/log/commit
│   ├── security.{h,cpp}          # path traversal, blocked commands, shell_escape
│   ├── project.{h,cpp}           # настройки проекта
│   ├── shell.h                   # безопасные shell-обёртки (timeout, quote, cap)
│   ├── module_api.{h,cpp}        # интерфейс модуля + ModuleRegistry
│   └── prompts.h                 # базовый системный промпт
├── modules/              # доменные модули
│   ├── wordpress/        # 12 инструментов + 6 inline-навыков
│   ├── python/           # 6 инструментов + 4 inline-навыка
│   └── devops/           # 11 инструментов + 3 inline-навыка
├── ui/coder_window.{h,cpp}  # окна «Проект», «Модули», «Инструменты» + agent-mode UI
├── skills/               # внешний навык wp_setup.md
├── tests/                # unit-тесты (48)
├── src/plugin_main.cpp   # точка входа (ll_plugin_init/render/shutdown, agent mode)
├── CMakeLists.txt
├── plugin.json
├── DEVELOPMENT_PLAN.md   # план развития (фазы 1–6)
└── CHANGELOG.md
```

## Инструменты и навыки

| Модуль | Инструменты | Навыки (inline) |
|--------|-------------|-----------------|
| **core** (базовые) | `read_file`, `write_file`, `search_replace`, `grep_search`, `repo_map`, `list_skills`, `skill_detail`, `exec_command`, `rag_index`, `rag_query` | — |
| **core** (git) | `git_status`, `git_diff`, `git_log`, `git_commit` | — |
| **WordPress** | `wp_cli`, `wp_db`, `wp_media`, `wp_option`, `wp_rest`, `wp_create_site`, `wp_check_deps`, `deploy`, `verify`, `php_lint`, `headless_render`, `validate` | `wp_theme`, `wp_hook`, `wp_database`, `wp_media`, `wp_plugin_boilerplate`, `wp_git` |
| **Python** | `python_run`, `pip_install`, `django_manage`, `pytest_run`, `venv_create`, `python_lint` | `python_django`, `python_flask`, `python_fastapi`, `python_project` |
| **DevOps** | `docker_build`, `docker_run`, `docker_ps`, `docker_logs`, `systemd_status`, `systemd_restart`, `nginx_test`, `nginx_reload`, `cron_list`, `cron_add`, `ssh_exec` | `devops_docker`, `devops_systemd`, `devops_nginx` |

Итого: **43 инструмента**, **13 навыков** (13 inline из модулей + 1 внешний `skills/wp_setup.md`).

## Как это работает

1. При старте плагин регистрирует модули через `ModuleRegistry`
2. Каждый модуль регистрирует инструменты в `ToolsRegistry` и навыки (inline)
3. `SkillsManager` собирает навыки: inline из модулей + `.md` из `skills/` (с дедупом по имени)
4. `Engine` собирает системный промпт: базовый + промпт выбранного модуля + активные навыки
5. ReAct-цикл (`AgentLoop`) вызывает инструменты и ведёт multi-turn сессию диалога
6. Прогресс и метрики видны в чате через `chat_event` и в окне «AI Coder»

## Безопасность

- **Пути:** `security::is_path_safe()` — запрет path traversal (`..`)
- **Опасные пути:** `security::is_path_not_dangerous()` — запрет записи в `/etc`, `/proc`
- **Команды:** `security::is_command_allowed()` — блок `rm -rf /`, `mkfs`, `dd` и т.д.
- **Разрешения:** доступ к файлам вне проекта требует подтверждения пользователя
- **Shell:** `shell::shell_quote()` — экранирование аргументов для shell

> ⚠️ **Известные проблемы безопасности (план исправления — Фаза 1 в DEVELOPMENT_PLAN.md):**
> Python и DevOps модули не используют `shell::shell_quote`. Инструменты `docker_run`,
> `ssh_exec`, `cron_add`, `pip_install` подвержены shell injection через аргументы от LLM.

## Сборка и деплой

```bash
# Конфигурация
cmake -S . -B build

# Тесты
cmake --build build --target wp_coder_tests -j$(nproc)
./build/tests/wp_coder_tests          # 48/48 PASS

# Плагин
cmake --build build --target wp_coder -j$(nproc)
#    артефакт: build/plugins/libwp_coder.so

# Деплой
cp build/plugins/libwp_coder.so plugins/libwp_coder.so
```

## Запуск

```bash
env -u LD_PRELOAD ./build/llama-gui-core --agent=ai_coder
```

Окна плагина (меню **AI Coder**):

| Окно | Горячая клавиша |
|------|-----------------|
| Проект | `Ctrl+Shift+W` |
| Модули | `Ctrl+Shift+M` |
| Инструменты | `Ctrl+Shift+T` |

## Текущее состояние

- ✅ Сборка проходит, `libwp_coder.so` (~670 Кб) собран
- ✅ 64/64 unit-тестов проходят
- ✅ D1 (разбивка `run_task` на компоненты) завершён
- ✅ D2 (парсер протокола `tool_protocol`) завершён
- ✅ `--agent=ai_coder` проверен вживую
- ✅ Фаза 1 (безопасность) — завершена: shell injection исправлены во всех модулях
- ✅ Фаза 2 (стабильность) — **вся завершена**: 2.0–2.6 (data race, дублирование, версия, retry, таймаут, FSM, headless_render)

Дальнейшие шаги — в `DEVELOPMENT_PLAN.md`.

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

## Разработка модулей

См. `MODULE_DEVELOPMENT.md` — гайд по созданию новых доменных модулей.
