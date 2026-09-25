# wp_coder — AI-кодер с модульной архитектурой

Плагин `wp_coder` для llama-gui — универсальный ReAct-агент с доменными модулями.
WordPress — один из модулей; Python и DevOps подключаются аналогично.

- **Версия:** 0.5.0 (см. `plugin.json` — единственный источник версии)
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
│   ├── project.{h,cpp}           # разрешение путей относительно корня проекта
│   ├── shell.h                   # безопасные shell-обёртки (timeout, quote, cap)
│   ├── module_api.{h,cpp}        # интерфейс модуля + ModuleRegistry
│   └── prompts.h                 # базовый системный промпт
├── modules/              # доменные модули
│   ├── wordpress/        # 12 инструментов + 6 inline-навыков
│   ├── python/           # 6 инструментов + 4 inline-навыка
│   └── devops/           # 11 инструментов + 3 inline-навыка
├── ui/coder_window.{h,cpp}  # окна «Проект», «Модули», «Инструменты», «Сессия» + agent-mode UI
├── skills/               # внешний навык wp_setup.md
├── tests/                # unit-тесты (138)
├── src/plugin_main.cpp   # точка входа (ll_plugin_init/render/shutdown, agent mode)
├── src/README.md         # почему в src/ только plugin_main.cpp
├── CMakeLists.txt
├── plugin.json
├── AGENT_PARITY_PLAN.md  # план паритета агентных возможностей с opencode CLI
├── DEVELOPMENT_PLAN.md   # план развития (фазы 1–6)
└── CHANGELOG.md
```

## Инструменты и навыки

| Модуль | Инструменты | Навыки (inline) |
|--------|-------------|-----------------|
| **core** (базовые) | `read_file`, `write_file`, `search_replace`, `edit_file`, `undo_edit`, `grep_search`, `repo_map`, `list_dir`, `web_fetch`, `exec_command`, `list_skills`, `skill_detail`, `rag_index`, `rag_query` | — |
| **core** (git) | `git_status`, `git_diff`, `git_log`, `git_add`, `git_branch`, `git_checkout`, `git_commit` | — |
| **WordPress** | `wp_cli`, `wp_db`, `wp_media`, `wp_option`, `wp_rest`, `wp_create_site`, `wp_check_deps`, `deploy`, `verify`, `php_lint`, `headless_render`, `validate` | `wp_theme`, `wp_hook`, `wp_database`, `wp_media`, `wp_plugin_boilerplate`, `wp_git` |
| **Python** | `python_run`, `pip_install`, `django_manage`, `pytest_run`, `venv_create`, `python_lint` | `python_django`, `python_flask`, `python_fastapi`, `python_project` |
| **DevOps** | `docker_build`, `docker_run`, `docker_ps`, `docker_logs`, `systemd_status`, `systemd_restart`, `nginx_test`, `nginx_reload`, `cron_list`, `cron_add`, `ssh_exec` | `devops_docker`, `devops_systemd`, `devops_nginx` |

Итого: **50 инструментов**, **14 навыков** (13 inline из модулей + 1 внешний `skills/wp_setup.md`).

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

> ✅ **Фаза 1 (безопасность) реализована:** все аргументы от LLM проходят
> `shell::shell_quote()`/`sanitize_ident()`/валидацию метасимволов во всех модулях
> (python, devops, wordpress). `exec_command` проверяется политикой
> `is_command_allowed()`.

## Сборка и деплой

```bash
# Конфигурация
cmake -S . -B build

# Тесты
cmake --build build --target wp_coder_tests -j$(nproc)
./build/tests/wp_coder_tests          # 138/138 PASS

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
| Сессия | `Ctrl+Shift+S` |

## Текущее состояние

- ✅ Сборка проходит, `libwp_coder.so` (~670 Кб) собран
- ✅ 138/138 unit-тестов проходят (core + модули + манифесты)
- ✅ D1 (разбивка `run_task` на компоненты) завершён
- ✅ D2 (парсер протокола `tool_protocol`) завершён
- ✅ `--agent=ai_coder` проверен вживую
- ✅ Фаза 1 (безопасность) — завершена: shell injection исправлены во всех модулях
- ✅ Фаза 2 (стабильность) — **вся завершена**: 2.0–2.6 (data race, дублирование, версия, retry, таймаут, FSM, headless_render)
- ✅ Фаза 3 (новые инструменты) — **вся завершена**: list_dir, web_fetch, edit_file, undo_edit, git_add/branch/checkout
- ✅ Фаза 4 (качество кода) — **вся завершена**: limits.h, json_utils.h, file_utils.h, тесты модулей, UI-фиксы
- ✅ Фаза 5 (UX) — 5.1–5.3: окно «Сессия», resume сессии, настройки агента (5.4 max_tokens — отложено, требует расширения хоста)
- ✅ Фаза 6 (документация) — **вся завершена**: теневые .md удалены, README/CHANGELOG/USAGE актуализированы

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

## Манифест

`plugin.json` — единственный источник версии и capabilities. Плагин
кладёт его в каталог сборки **под своим именем** (`wp_coder.json`),
потому что `PluginManager` ищет манифест как `<имя_плагина>.json` и
только потом — общий `plugin.json`. Общее имя в каталоге, где лежат
несколько плагинов, принадлежит им всем: кто записал последним, тот и
«владелец», а плагин без своего манифеста подхватывает чужой. Хост
дополнительно **отбрасывает** манифест, у которого `name` не совпадает
с именем плагина.

## Режимы

Ограничения режимов проверяет ядро по флагам инструментов
(`ToolFlags`, `core/tool.h`) — не сам инструмент и не текст промпта.
Инструмент, не помеченный `read_only`, в Research и в режиме
«сначала план» не вызывается.

- **Code** — полный доступ ко всем инструментам
- **Research** — только чтение: пропускаются только инструменты с флагом
  `read_only` (в том числе `read_file`, `grep_search`, `repo_map`,
  `git_status`, `web_fetch`). Запись файлов, запуск команд и
  необратимые операции (`write_file`, `exec_command`, `deploy`,
  `git_commit`, `docker_run`, `wp_cli`, `pip_install`, …) запрещены
- **План** («сначала план») — запуск команд и необратимые операции
  запрещены, но правки файлов не блокируются: они сохраняются как
  предложения и применяются только после подтверждения в UI
- **Review** — после правок автоматически запускает `verify`

## Разработка модулей

См. `MODULE_DEVELOPMENT.md` — гайд по созданию новых доменных модулей.
