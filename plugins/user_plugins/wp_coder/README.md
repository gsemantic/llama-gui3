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
│   ├── agent_components.{h,cpp}  # SessionStore, PermissionGate, ToolRunner, Planner, AgentLoop (D1)
│   ├── tools_registry.{h,cpp}    # Динамический реестр инструментов
│   ├── skills_manager.{h,cpp}    # Навыки: inline из модулей + .md из skills/
│   ├── tool_protocol.{h,cpp}     # Парсер wp_action / JSON tool_calls (D2)
│   ├── base_tools.{h,cpp}        # read/write/search_replace/repo_map/grep/exec/rag
│   ├── git_tools.{h,cpp}         # git_status/diff/log/commit
│   ├── security.{h,cpp}          # path traversal, blocked commands, shell_escape
│   ├── project.{h,cpp}           # настройки проекта
│   ├── module_api.{h,cpp}        # интерфейс модуля + ModuleRegistry
│   └── prompts.h                 # базовый системный промпт
├── modules/              # доменные модули
│   ├── wordpress/        # 12 инструментов + 6 inline-навыков
│   ├── python/           # 6 инструментов + 4 inline-навыка
│   └── devops/           # 11 инструментов + 3 inline-навыка
├── ui/coder_window.{h,cpp}  # окна «Проект», «Модули», «Инструменты» + agent-mode UI
├── skills/               # внешние .md-навыки (загружаются через load_from_directory)
├── tests/                # unit-тесты (48)
├── src/plugin_main.cpp   # точка входа (ll_plugin_init/render/shutdown, agent mode)
└── CMakeLists.txt, plugin.json
```

## Инструменты и навыки

| Модуль | Инструменты | Навыки (inline) |
|--------|-------------|-----------------|
| **core** (базовые) | `read_file`, `write_file`, `search_replace`, `grep_search`, `repo_map`, `list_skills`, `skill_detail`, `exec_command`, `rag_index`, `rag_query` | — |
| **core** (git) | `git_status`, `git_diff`, `git_log`, `git_commit` | — |
| **WordPress** | `wp_cli`, `wp_db`, `wp_media`, `wp_option`, `wp_rest`, `wp_create_site`, `wp_check_deps`, `deploy`, `verify`, `php_lint`, `headless_render`, `validate` | `wp_theme`, `wp_hook`, `wp_database`, `wp_media`, `wp_plugin_boilerplate`, `wp_git` |
| **Python** | `python_run`, `pip_install`, `django_manage`, `pytest_run`, `venv_create`, `python_lint` | `python_django`, `python_flask`, `python_fastapi`, `python_project` |
| **DevOps** | `docker_build`, `docker_run`, `docker_ps`, `docker_logs`, `systemd_status`, `systemd_restart`, `nginx_test`, `nginx_reload`, `cron_list`, `cron_add`, `ssh_exec` | `devops_docker`, `devops_systemd`, `devops_nginx` |

Итого: **43 инструмента**, **14 навыков** (13 inline из модулей + 1 внешний `skills/wp_setup.md`).

> **Про навыки:** менеджер дедуплицирует по имени — inline-навыки модулей имеют
> приоритет над `.md`. Поэтому 6 файлов в `skills/`
> (`wp_database.md`, `wp_git.md`, `wp_hook.md`, `wp_media.md`,
> `wp_plugin_boilerplate.md`, `wp_theme.md`) сейчас **теневые** (не загружаются:
> их имена уже заняты inline-версиями). Реально из `skills/` подхватывается
> только `wp_setup.md`.

## Как это работает

1. При старте плагин регистрирует модули через `ModuleRegistry`
2. Каждый модуль регистрирует инструменты в `ToolsRegistry` и навыки (inline)
3. `SkillsManager` собирает навыки: inline из модулей + `.md` из `skills/` (с дедупом по имени)
4. `Engine` собирает системный промпт: базовый + промпт выбранного модуля + активные навыки
5. ReAct-цикл (`AgentLoop`) вызывает инструменты и ведёт multi-turn сессию диалога
6. Прогресс и метрики видны в чате через `chat_event` и в окне «AI Coder»

## Сборка и деплой

```bash
# 0. Конфигурация (один раз; системные nlohmann_json + curl уже подключены)
cmake -S . -B build

# 1. Тесты
cmake --build build --target wp_coder_tests -j$(nproc)
./build/tests/wp_coder_tests          # 48/48 PASS

# 2. Плагин
cmake --build build --target wp_coder -j$(nproc)
#    артефакт: build/plugins/libwp_coder.so

# 3. Деплой (приложение подхватывает из plugins/)
cp build/plugins/libwp_coder.so plugins/libwp_coder.so
```

Плагин **не** линкует ядро приложения — только SDK (`include/plugins/plugin_api.h`),
ImGui (символы из exe) и `headless_browser` (статически, опционально).

## Запуск

```bash
# локальный LLM-сервер (например llama-server на 8081)
env -u LD_PRELOAD ./build/llama-gui-core --agent=ai_coder
```

Окна плагина (меню **AI Coder**):

| Окно | Горячая клавиша |
|------|-----------------|
| Проект | `Ctrl+Shift+W` |
| Модули | `Ctrl+Shift+M` |
| Инструменты | `Ctrl+Shift+T` |

## Текущее состояние (2026-09-10)

- ✅ Сборка проходит, `libwp_coder.so` (~670 Кб) собран и задеплоен
- ✅ 48/48 unit-тестов проходят
- ✅ D1 (разбивка `run_task` на компоненты) завершён
- ✅ `--agent=ai_coder` проверен вживую: агент получает сообщение,
  multi-turn LLM (3 вызова через cloud/agnes-3.0-flash), отвечает корректно

Дальнейшие шаги — в `REFACTOR_PLAN.md` (фазы D3–E).