# wp_coder — прогресс разработки (AI-кодер для WordPress)

> Статус на 2026-08-26. Плагин собран (`build/plugins/libwp_coder.so`) и задеплоен
> рядом с исполняемым файлом (`build/plugins/wp_coder.json`). Загружается автоматически
> при старте `build/llama-gui-core` (меню **WordPress**).

## Что реализовано (план выполнен по Фазам 0–5)

| Фаза | Содержание | Статус |
|---|---|---|
| 0 | MVP-каркас: окна «Проект»/«Агент», worker-поток с ReAct-циклом, `read/write_file`, `grep_hooks`, `php_lint`, `rag_index/query` | ✅ |
| 1 | Инструменты локали: `wp_cli`, `headless_render` (Chromium DOM + thin-content) | ✅ |
| 2 | Знания WP: `repo_map` (компактный обзор файлов/функций/хуков) | ✅ |
| 3 | Удалённый WP + деплой: `wp_rest` (app_password Basic-auth), `validate` (`php -l`), `deploy` (rsync или внешний `deploy.sh`) | ✅ |
| 4 | Безопасность правок: план-режим (правки не применяются сразу, список с «Применить»/«Отклонить») | ✅ |
| 5 | Навыки (skills, как в opencode), ролевые режимы Code/Research/Review, `verify` (php -l + HTTP-статус + рендер лок. сайта) | ✅ |

## Архитектура (файлы в `plugins/user_plugins/wp_coder/`)

- `src/wp_coder.h` — состояние `WpCoderState`, структуры `AgentEvent`/`PendingWrite`/`Skill`, прототипы.
- `src/plugin_main.cpp` — `ll_plugin_init/render/shutdown`, меню, окна ImGui, UI «Проект»/«Агент».
- `src/agent.cpp` — worker-поток: системный промпт + цикл `llm_complete_ex` → парсинг блока `wp_action` → `tool_run` → RESULT обратно в модель (до 12 шагов). Настройки `setting_get/set_str`.
- `src/tools.cpp` — реализация инструментов (см. список ниже) + `pending_apply/pending_discard`.
- `src/project.cpp` — `project_load/save_settings`, `project_detect_php`, `project_resolve`, `skills_load` (читает `*.md` из `<data_dir>/wp_coder/skills` и `skills/`).
- `skills/wp_hook.md`, `wp_theme.md`, `wp_plugin_boilerplate.md` — поставляемые навыки.
- `CMakeLists.txt`, `plugin.json`.

### Протокол инструментов (запомнить для правок агента)
Модель выводит РОВНО один fenced-блок, который парсит `parse_action()` в `agent.cpp`:
```
```wp_action
TOOL: read_file
PATH: wp-content/themes/x/functions.php
```
```
Поля: `TOOL`, `PATH`, `ROOT`, `QUERY`, `PATTERN`, `K`, `CLI`, `URL`, и `CONTENT_BEGIN … CONTENT_END` для `write_file`.
Доступные `TOOL`: `read_file`, `write_file`, `grep_hooks`, `php_lint`, `wp_cli`,
`headless_render`, `rag_index`, `rag_query`, `repo_map`, `wp_rest`, `validate`,
`verify`, `deploy`, `list_skills`.

### Потокобезопасность
`llm_complete_ex`/`rag_*` дёргаются из worker-потока; UI (`ll_plugin_render`) только
читает `g_state` под `std::mutex`. Хост-API не гарантирует thread-safety — если при
прогоне вживую будут гонки, вынести вызовы LLM в очередь на главный поток.

## Сборка и деплой

```bash
cmake -S . -B build                 # конфигурация (нужны nlohmann_json+curl в системе;
                                    #   FetchContent не сработал, т.к. они системные)
cmake --build build --target wp_coder
# артефакт:
cp plugins/user_plugins/wp_coder/plugin.json build/plugins/wp_coder.json
# запуск: ./run_gui.sh  (cwd=build, сканирует build/plugins/)
```
Плагин НЕ линкует ядро приложения — только SDK (`include/plugins/plugin_api.h`) + ImGui
(символы из exe) + `headless_browser` (статически).

## Правка хоста (для AST-чанкинга PHP)
- `src/core/ast_parser.cpp` — слабые символы `tree_sitter_php/javascript/css/html` в
  `grammars()` (null-фильтрация). Без грамматик язык не регистрируется → RAG для `.php`
  использует text-fallback.
- `CMakeLists.txt:98` — `TS_LANGUAGES` дополнен web-стеком.
- Чтобы включить настоящий AST-чанкинг: положить исходники грамматик в
  `deps/tree-sitter-php-src/src/{parser.c,scanner.c}` (и аналогично js/css/html) и пересобрать.
  **Важно:** ABI tree-sitter — грамматика и `libtree-sitter` в системе должны совпадать
  по версии (в системе 0.6.3 — старый; проверить совместимость при добавлении).

## Открытые задачи / следующие шаги (для продолжения)
1. **tree-sitter-php в хост** — подложить грамматику в `deps/`, проверить ABI с системным
   libtree-sitter (0.6.3 может быть слишком старым; возможно, пересобрать libtree-sitter).
2. **Живой прогон** — запустить GUI, задать в «Проект» корень локального WP
   (apache2+MariaDB), `wp_local_url` (http://localhost:порт), проверить агента на реальной задаче.
3. **Полная сборка ядра** — проверить, что правка `ast_parser.cpp` линкуется в `llama-gui-core`
   (объект компилируется, но полный линк ядра не прогонялся).
4. **ftp/sftp-деплой** — сейчас `deploy` для ftp/sftp вызывает внешний `deploy.sh` в корне
   проекта (по образу `news_rewriter/deploy.sh`); можно встроить напрямую (curl sshpass/sftp).
5. **Настоящая многоАгентность** — сейчас «роли» реализованы режимами в одном потоке.
   Опционально: под-агенты в отдельных потоках (research / coder / review) с оркестратором.
6. **Авто-тесты через WP** — расширить `verify`: `wp --path=… plugin activate`, codeception/phpunit,
   сравнение headless-DOM до/после правки.
7. **Навыки пользователя** — писать `.md` в `<data_dir>/wp_coder/skills` (путь подскажет
   `path_data_dir` хоста).

## Ключевые ограничения среды (зафиксировано)
- При разработке GitHub кратковременно был недоступен → сначала только `-fsyntax-only`;
  позже собрано целиком. Сейчас сеть есть, системные `nlohmann_json`/`curl` присутствуют.
- `headless_browser` требуетChromium в PATH (проверяется в рантайме через `available()`).
