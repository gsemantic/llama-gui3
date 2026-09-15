# План развития AI-кодера (wp_coder) v0.4+

Статусы: `[ ]` не начато · `[~]` в работе · `[x]` готово

Обновлять статус при каждом изменении. После каждого шага — сборка + тесты.

---

## Фаза 1 — Критическая безопасность ( blockers )

> Без этих исправлений плагин опасен для продакшена: LLM может выполнить
> произвольные команды через неэкранированные shell-аргументы.

- [x] 1.1. **Python модуль: заменить `run_capture` на `shell::run_capture`**
  - Файл: `modules/python/python_tools.cpp`
  - Удалить локальную `run_capture()`, использовать `shell::run_capture` + `shell::shell_quote`
  - `pip_install`: экранировать `a.query` через `shell::shell_quote`
  - `python_run`: `shell::shell_quote(abs)` вместо `\"...\"`
  - `django_manage`: экранировать `args` через `shell::shell_quote`
  - `pytest_run`: экранировать `path` и `marker`

- [x] 1.2. **DevOps модуль: заменить `run_capture` на `shell::run_capture`**
  - Файл: `modules/devops/devops_tools.cpp`
  - Удалить локальную `run_capture()`, использовать `shell::run_capture`
  - `docker_build`: `shell::shell_quote(ctx)`, `shell::shell_quote(file)`
  - `docker_run`: `shell::shell_quote(a.cli)` — это самый опасный: CLI от модели
  - `docker_logs`: `shell::shell_quote(a.query)`
  - `systemd_status/restart`: `shell::shell_quote(a.query)` (санитизировать имя сервиса: `sanitize_ident`)
  - `nginx_test`: `shell::shell_quote(a.query)`
  - `cron_list`: `shell::shell_quote(a.query)` + `sanitize_ident`
  - `cron_add`: пересмотреть — вместо `echo '...' | crontab -` собрать файл и загрузить
  - `ssh_exec`: `shell::shell_quote(a.cli)` — критично, CLI от модели уходит по SSH

- [x] 1.3. **WP модуль: экранирование shell-аргументов**
  - Файл: `modules/wordpress/wp_tools.cpp`
  - `wp_cli`: аргументы `a.cli` — частично экранировать или валидировать через whitelist символов
  - `deploy`: `shell::shell_quote(target)`, `shell::shell_quote(st.deploy_remote_dir)`
  - `wp_create_site`: SQL injection в CREATE USER — использовать `shell::shell_quote` для db_name/user/pass

- [x] 1.4. **`sanitize_ident` — расширить на все модули**
  - Вынести из `wp_tools.cpp` в `core/security.h` как `security::sanitize_ident()`
  - Использовать в DevOps для имён контейнеров, сервисов
  - Добавить тест в `test_security.cpp`

- [x] 1.5. **`shell::run_capture` — возвращать exit code**
  - Расширить: `run_capture` возвращает struct `{std::string output, int exit_code}`
  - Или добавить перегрузку `run_capture_ex(cmd, timeout, &exit_code)`
  - Обновить все вызовы, где exit code важен (validate, wp_check_deps и т.д.)

---

## Фаза 2 — Стабильность и надёжность

- [x] 2.0. **Баг: `load_settings()` не загружает `deploy_remote_dir`**
  - Файл: `core/engine.cpp`
  - Поле сохраняется в `save_settings()`, но отсутствует в `load_settings()`
  - После перезапуска путь деплоя теряется
  - Добавить: `state_.deploy_remote_dir = setting_get(cb_, "wp_coder.deploy_remote_dir", "");`

- [x] 2.0a. **Баг: `SessionStore::messages()` — data race**
  - Файл: `core/agent_components.h`
  - `messages()` возвращает `const&` к `state_.session` без мьютекса
  - Если worker-поток пишет в session, а UI читает через `messages()` — UB
  - Исправление: убрать `messages()` или вернуть копию под мьютексом

- [x] 2.0b. **Баг: дублирование `check_external_permission`**
  - `Engine::check_external_permission()` и `PermissionGate::check()` — идентичный код
  - Аналогично `permission_allow_once/always/reject` дублированы
  - Оставить только `PermissionGate`, из Engine убрать или сделать thin delegate

- [x] 2.0c. **Баг: `llm_chat` лямбда захватывает `cb` по значению**
  - Файл: `src/plugin_main.cpp`
  - `cb.llm_chat = [cb](...)` — копирует `cb` на момент инициализации
  - При изменении `cb.llm_complete` после захвата — лямбда не увидит изменений
  - Исправление: захватить `g_api`/`g_host` напрямую (они уже глобальны)

- [x] 2.0d. **Баг: версия в `ll_plugin_info` = 0.2.0, plugin.json = 0.3.0**
  - Файл: `src/plugin_main.cpp`
  - Заменить hardcoded "0.2.0" на "0.3.0"

- [x] 2.0e. **Удалить `.bak` файлы**
  - `core/engine.cpp.bak` (37K) — удалён ✅
  - `core/engine.cpp.bak2` (19K) — удалён ✅

- [x] 2.1. **Retry для LLM-вызовов (429/5xx)**
  - Файл: `plugin_main.cpp` (в `cb.llm_chat`) + `src/plugins/plugin_manager.cpp`
  - При ошибке HTTP 429/5xx — 3 попытки с backoff (1s, 3s) — задержек между 3 попытками ровно 2
  - Хост теперь возвращает `{"ok":0,"error":"..."}` вместо `nullptr` — плагин видит тип ошибки
  - Retryable: 429, 500/502/503/504, 5xx, таймаут, «исчерпан», null-ответ
  - Ожидание прерывается кнопкой «Стоп» (проверка `abort_requested` каждые 100 мс)
  - Логирование retry в `push_event(AgentEvent::Status)`
  - **Retry-After**: не реализован — хост не передаёт header (нет парсинга в http_client);
    `OpenRouterClient::complete()` уже ретраит сам (3 попытки, 3000/5000 мс)

- [x] 2.2. **Прерывание текущего LLM-вызова**
  - **API `llm_chat_cancel` в хосте НЕ существует** — проверено, `LlamaHostApi` не содержит
  - Реализован «пока»-вариант: LLM-вызов в `std::async` + таймаут (по умолчанию 120 с)
  - Настройка `wp_coder.llm_timeout_ms` (EngineState::llm_timeout_ms, load/save_settings)
  - «Стоп» (`abort_requested`) проверяется каждые 100 мс — ожидание прерывается мгновенно
  - Таймаут не retryable: возвращаем ошибку, бросаем async-поток (он доработает в фоне)
  - Захват копий строк в async-лямбду — защита от use-after-free при брошенном потоке
  - Ограничение: отменённый запрос всё равно дойдёт до провайдера (хост не умеет cancel)

- [x] 2.3. **Состояние агента как FSM (D3 из REFACTOR_PLAN)**
  - States: `AgentState` enum: Idle → Planning → Executing ⇄ WaitingPermission → Done/Aborted
  - Добавлен `agent_state_name()` (человекочитаемые имена для UI)
  - Добавлен `Engine::set_state()` — публикует observer-событие `state: <имя>` при реальном переходе
  - Убраны флаги `running`, `waiting_for_permission` — заменены на `state_.state`
  - `shutting_down` оставлен отдельно: это lifecycle-флаг движка (stop()), а не состояние агента
  - `waiting_in_sync` оставлен: означает «синхронное ожидание разрешения» (для chat-режима)
  - wait()-предикаты включают `abort_requested` — «Стоп» будит ожидание разрешения
  - UI: спиннер показывает имя состояния («План», «Выполнение», «Ожидание разрешения»)
  - Тест: `engine_fsm_state_transitions` (переходы, события, имена, no-spam)

- [x] 2.4. **Версия plugin_info = plugin.json** (дубликат 2.0d — уже сделано)
  - `ll_plugin_info` возвращает "0.3.0", синхронно с plugin.json
  - При будущих подъёмах версии обновлять оба места

- [x] 2.5. **Удалить `.bak2` файл** (дубликат 2.0e — уже сделано)
  - `core/engine.cpp.bak2` удалён вместе с `.bak`

- [x] 2.6. **`headless_render` — реальная интеграция (Вариант A)**
  - Подключена библиотека `libs/headless_browser` (линьковка цели headless_browser)
  - Исправлен CMake: блок `if(NOT TARGET headless_browser)` пропускался при сборке
    в составе корневого проекта → линкуем target напрямую, fallback на find_library
  - `render_dom()` → сериализованный DOM после выполнения JS (timeout 30 с)
  - `is_thin_content()` → диагностика «белого экрана» / SPA-оболочки без рендера
  - Вывод обрезан до kWpMaxOutput (8000 символов)
  - Если chromium не найден — внятная ошибка (не заглушка)
  - Проверено: chromium в системе рендерит DOM (headless --dump-dom) ✅

---

## Фаза 3 — Новые инструменты

- [x] 3.1. **`list_dir` — лёгкий ls каталога**
  - В отличие от `repo_map` (тяжёлый, с символами) — просто список файлов/каталогов
  - PATH: каталог, опционально K: лимит (по умолчанию 100)
  - Регистрация в `base_tools.cpp`

- [x] 3.2. **`web_fetch` — HTTP GET запрос**
  - URL: адрес
  - Использовать curl через `shell::run_capture` (`curl -s -L -m 30 --max-redirs 3`)
  - С лимитом вывода (kMaxToolOutput), ограничение redirects — `--max-redirs 3`
  - Регистрация в `base_tools.cpp`

- [x] 3.3. **`edit_file` — правка по строкам (line-range replace)**
  - PATH: файл, K: start_line (1-based), QUERY: end_line (1-based, включительно), CONTENT: новый текст
  - Надёжнее `search_replace` при множественных вхождениях (заменяет по позиции, а не по совпадению)
  - Поддержка plan_mode, backup (.orig) для undo_edit
  - Регистрация в `base_tools.cpp`

- [x] 3.4. **`git_add` / `git_branch` / `git_checkout`**
  - Частичные коммиты: `git_add PATH` (пусто = все), `git_commit` также принимает PATH
  - `git_branch` QUERY (пусто = список), `git_checkout` QUERY — имя ветки
  - Регистрация в `git_tools.cpp`; общий helper `git_run()`

- [x] 3.5. **`undo_edit` — отмена последней правки**
  - При `write_file`/`search_replace`/`edit_file` — сохраняется backup `.orig` (helper `backup_file()`)
  - `undo_edit` PATH: файл — восстановить из backup
  - Лимит: хранится только последний backup на файл (перезаписывается)

---

## Фаза 4 — Качество кода и устранение дублирования

- [x] 4.1. **Устранить дублирование `run_capture`**
  - Три копии: `shell.h`, `python_tools.cpp`, `devops_tools.cpp`
  - Фаза 1.1 и 1.2 уже заменяют на `shell::run_capture`
  - Осталась копия в `project.cpp` (`bool run_capture`) — заменена на `shell::run_capture_status`

- [x] 4.2. **Дублирование `walk_php` / `walk_all`**
  - `base_tools.cpp` и `wp_tools.cpp` содержат почти идентичные функции
  - Вынесены в `core/file_utils.h` как `file_utils::walk_files()` с параметром skip-каталогов и расширения

- [x] 4.3. **Дублирование `resolve_path`**
  - В `base_tools.cpp` и `python_tools.cpp` — вынесено в `core/project.h` (`project_resolve`)

- [x] 4.4. **Тесты для модулей (WordPress, Python, DevOps)**
  - Добавлены: `test_wp_tools.cpp`, `test_python_tools.cpp`, `test_devops_tools.cpp` (+21 тест, всего 86)
  - Модульные .cpp компилируются в тестовый бинарник (`${MODULE_SOURCES}` + линковка headless_browser)
  - Тесты безопасности: shell injection в wp_cli/docker_run, DDL-запрет wp_db, санитизация имён
  - Тестируются только ветки валидации (без запуска реальных команд)
  - Исправлен баг: `docker_run` блокировал только `$(` — теперь `; | & \` $ > <`

- [x] 4.5. **Константы лимитов в одном месте**
  - Вынесены в `core/limits.h`: kMaxToolOutput, kModuleMaxOutput, kMaxGrepMatches, kReadFileChars, kMaxSymFile, kMaxSteps, kSessionBudget, kResultBudget
  - `kSessionBudget` дублировался (engine.cpp + agent_components.cpp), `kMaxSteps` (8 vs 12) — устранено

- [x] 4.6. **Дублирование JSON-парсинга**
  - `json_str`/`json_int` в `tool_protocol.cpp` и `find_str`/`find_int` в `plugin_main.cpp`
  - Вынесены в `core/json_utils.h` (`json::str`, `json::int_`)

- [x] 4.7. **UI: race condition для pending-правок**
  - Копия `st.pending` под мьютексом, работа по целостному снимку

- [x] 4.8. **UI: дублирование кода сохранения настроек**
  - 12 полей копировались в двух кнопках «Сохранить»
  - Вынесены в `apply_settings_from_buffers()` (с записью под мьютексом)

---

## Фаза 5 — UX / Продукт (из REFACTOR_PLAN E1–E4)

- [ ] 5.1. **Окно «Сессия» (E1)**
  - Список шагов: tool, args, статус, токены
  - Кнопка отмены конкретного шага
  - Рендерить в `render_extras()` или отдельное окно

- [ ] 5.2. **Resume сессии (E2)**
  - Сохранение сессии в JSON на диск при прерывании
  - Загрузка при старте — продолжение с того же места
  - Файл: `<data_dir>/wp_coder/session.json`

- [ ] 5.3. **Настройки агента (E3)**
  - Лимит шагов (сейчас hardcoded kMaxSteps=12)
  - Бюджет токенов
  - Модель/провайдер
  - UI: в окне «Проект» или отдельная вкладка

- [ ] 5.4. **max_tokens из настроек (E4)**
  - Сейчас hardcoded в llm_chat callback
  - Добавить `EngineState::max_tokens` с default 4096
  - UI: InputInt в настройках

---

## Фаза 6 — Документация

- [ ] 6.1. **Слить теневые .md навыки с inline-версиями, затем удалить**
  - 6 файлов в `skills/` теневые (перекрыты inline), **но их контент отличается** — часто .md подробнее:
    - `wp_theme.md` — добавляет `get_template_part`, `wp_enqueue_style/wp_enqueue_script`
    - `wp_hook.md` — описывает приоритет add_action, shortcode callback `$atts/$content/$tag`
    - `wp_database.md` — добавляет `wp db prefix`, `wp db optimize`
    - `wp_media.md` — **существенно отличается**: добавляет `image-size`, `meta get`, `regenerate`
    - `wp_plugin_boilerplate.md` — **существенно отличается**: ABSPATH-проверка, запрет `wp_`-префикса
    - `wp_git.md` — **полностью другой набор рекомендаций**: git status/diff/log, осмысленные коммиты
  - План: перенести уникальный контент из .md в C++ строки inline-навыков, затем удалить .md
  - Оставить только `wp_setup.md` (единственный реальный внешний навык)
  - Обновить README: «13 навыков» (13 inline + 1 внешний)

- [ ] 6.2. **Обновить README.md**
  - Актуализировать таблицу инструментов (после добавления новых)
  - Добавить секцию «Безопасность»
  - Добавить секцию «Настройка» (агент, лимиты, модель)
  - Убрать дубли с USAGE.md — либо объединить, либо чётко разделить ответственности

- [ ] 6.3. **Обновить CHANGELOG.md**
  - Добавить секцию [0.4.0] с планируемыми изменениями по мере реализации

- [ ] 6.4. **Удалить USAGE.md если дублирует README**
  - Или наоборот: README = краткий обзор, USAGE = подробное руководство

---

## Приоритет выполнения

```
Фаза 1 (безопасность) → Фаза 2 (стабильность) → Фаза 4 (качество)
  → Фаза 3 (новые инструменты) → Фаза 5 (UX) → Фаза 6 (документация)
```

Фаза 1 — **обязательный первый шаг**. Без неё нельзя выпускать плагин.
Фазы 2+ — могут выполняться параллельно в разумных пределах.

## Контрольные точки

После каждой фазы:
1. `cmake --build build --target wp_coder_tests && ./build/tests/wp_coder_tests` — все тесты PASS
2. `cmake --build build --target wp_coder` — сборка плагина
3. Обновить статус в этом файле
4. Коммит с тегом `wp_coder/v0.X.Y`
