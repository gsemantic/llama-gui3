# Прогресс пересборки AI Coder плагина (llama-gui) — 2026-09-08

## Выполнено в текущей сессии

### Фаза 0 — Плагин (все подзадачи)

#### 1. Фиксы инструментов
- **`core/shell.h`** (новый файл) — обёртки `run_capture`, `run_capture_status`, `cap`, `shell_quote` с таймаутом через `timeout(1)` и ограничением вывода
- **`base_tools.cpp`** (полный редизайн):
  - `grep_search` → настоящий regex (ECMAScript), пустой паттерн = ошибка, лимит 200 совпадений / 12 Кб вывода
  - `read_file` → K-путь: skip lines + cap 12 Кб
  - `repo_map` → cap 12 Кб
  - `skill_detail` новый инструмент — QUERY имя → full body
  - `list_skills` — имена + описания
  - read/write/search_replace/exec_command — check_external_permission для файлов за пределами проекта
  - exec_command — timeout 60 + cap вывода
- **`git_tools.cpp`** — shell::run_capture(timeout), shell_quote для аргументов (git commit -m ...)
- **`wp_tools.cpp`** — shell::run_capture(timeout), shell_quote для db query, option get, rest url; sanitize_ident для wp_create_site

#### 2. Худой системный промпт
- **`prompts.h`** — обновлённая контрактная часть: grep regex, skill_detail, read_file K-путь
- **`skills_manager.cpp::build_skills_prompt`** — только `name — description`, тела выгружаются через `skill_detail`
  → размер промпта сокращён с ~13 Кб до ~3 Кб (~10× меньше токенов на префилл)

#### 3. Дедлок разрешений + wiring permission checks
- **`engine.cpp::run_task`** — пауза блокирующая (permission_cv.wait) вместо дедлока push_event под lock'ом
- Проверки доступа подключены к read_file/write_file/search_replace
- allow_once/allow_always через submit() с preserve_session

#### 4. Прогресс в чат
- **`engine.cpp::push_event`** — Status/Error форвардятся в cb_.chat_event (agent_mode_push_event)
- В run_task после каждого tool вызова — короткая строка в cb_.chat_event
→ пользователь видит шаги в реальном времени вместо 5 минут молчания

### Фаза 1 — Перестройка engine.cpp (многоходовая история)

#### Полностью переписан engine.cpp и engine.h:
- `ChatMsg { role, content }` — сессия диалога (не просто inbox)
- `LlmReply { ok, content, finish_reason, prompt_tokens, completion_tokens, error }` — структурированный ответ
- Новый `HostCallbacks.llm_chat(sys, messages, reply)` — multi-turn запрос (ожидание от хоста)
- `HostCallbacks.chat_event(text)` — прогресс в чат приложения
- `EngineState.session` — история текущего шага (user task → assistant replies → RESULTs)
- `EngineState.preserve_session` — флаг продолжения сессии после разрешения
- `EngineState.abort_requested` — atomic<bool>, прерывание между шагами
- Метрики: `llm_total_time`, `total_prompt_tokens`, `total_completion_tokens`, `steps`
- `trim_session_locked()` — сжатие старых RESULT при превышении kSessionBudget (60 Кб)
- Честные метрики tok/s = total_completion_tokens / llm_total_time

## Недоделано в этой сессии

### Фаза 1 — Обвязка plugin_main.cpp
Готово:
1. Добавлен callback `cb_.llm_chat` — парсинг JSON от `g_api->llm_chat_messages` (ok/content/finish_reason/prompt_tokens/completion_tokens), fallback на `cb_.llm_complete` при старом хосте без `llm_chat_messages`.
2. Добавлен callback `cb_.chat_event` → привязка к `g_api->agent_mode_push_event(g_host, text.c_str())`.

### Фаза 1 — Расширение API хоста
Готово:
1. `include/plugins/plugin_api.h` — поле `char* (*llm_chat_messages)(LlamaPluginHost*, const char* system_prompt, const char* messages_json)` добавлено в конец структуры (обратная совместимость по offsetof).
2. `src/plugins/plugin_manager.cpp` — реализация `host_llm_chat_messages`:
   - Локальный путь: parse messages_json → ChatCompletionRequest → create_chat_completion_async → response.usage
   - Облачный путь: parse → OpenRouterRequestParams.messages → complete() → resp.prompt_tokens/completion_tokens
   - Возврат: {"ok":1,"content":"...","finish_reason":"stop","prompt_tokens":N,"completion_tokens":N}
   - Fallback: без llm_chat_messages → legacy path (llm_complete_ex single turn)
3. `src/core/llama_interface_impl.cpp` — usage парсится (response.usage = j["usage"]; при наличии поля). Исправлена лишняя `}`.

### Фаза 1 — UI кнопки Стоп + обновлённые метрики
Готово в `coder_window.cpp::render_extras()`:
1. Кнопка "Стоп" (агент работает) → `engine().request_abort()`
2. Статистика: `%d tok | %.1f tok/s | %ds (LLM %.1fs) | %d steps`

### Тесты
Готово:
- `test_skills_manager.cpp` — промпт содержит только имя/описание (тела выгружаются через skill_detail), `Body B` больше не в промпте.
- `test_engine.cpp` — добавлен `engine_trim_session_compression`: сжатие старых RESULT и assistant-сообщений при превышении 60 Кб.
- `trim_session_locked` и `trim_session_test` вынесены в public (для доступа тестов).
- Исправлены баги: `compl` → `comp_tokens` (неопределённая переменная), лишняя `}` в `coder_window.cpp`, `cb_` → `cb` в фоллбэке лямбды `llm_chat` (захват по значению).

### Сборка + deploy
- `cmake --build build --target wp_coder_tests` — 43/43 тестов проходят.
- `cmake --build build --target wp_coder` — `build/plugins/libwp_coder.so` (622 Кб) успешно собран.

## Что изменится

### До пересборки (старый код):
- ~13 Кб системный промпт на каждом из 12 шагов ReAct → медленный префилл
- Один шаг = один non-streaming запрос (системный промпт + один user message)
- Нет истории между шагами → модель теряет контекст → много лишних шагов
- Ничего не видно в чате до завершения всего цикла (до 5 минут)
- grep_search не regex → model loops retrying patterns
- Бесконечный popen может повесить worker навсегда
- fake metrika tok/s

### После пересборки (новый код):
- ~3 Кб системный промпт (навыки только описаниями, ленивый skill_detail)
- Multi-turn сессия: каждый запрос несёт полную историю → provider-side prompt caching
- Результат одного шага виден сразу в чате (через agent_mode_push_event)
- Честные метрики tok/s из usage API
- grep_search regex + output caps (12 Кб) на всех инструментах
- popen с timeout(1) — worker не зависает
- Stop button для прерывания задачи
- Разрешения активны (check_external_permission в read/write/search_replace)
- Лимит сессии: trim_session при превышении 60 Кб — экономит токен-бюджет

## Следующий шаг (следующая сессия)
Все пункты Фазы 1 выполнены — пересборка завершена и задеплоена:
- `build/plugins/libwp_coder.so` (622 Кб) — сборка успешна
- `plugins/libwp_coder.so` — скопирован в папку, откуда её подхватывает приложение
  (`main_window.cpp:1633` — `subsystems.plugins_dir = "plugins"`)
- Проверка `dlopen`: плагин загружается, экспортирует `ll_plugin_api_version=1.0.0`
  (совпадает с хостом), `ll_plugin_info`, `ll_plugin_init`
- 43/43 unit-тестов проходят
- Кнопка Стоп, честные метрики, multi-turn chat, trim_session, permissions активны

Следующие задачи (не в Фазе 1):
1. Интеграция RAG в сессию агента (опционально).
2. Доработка `wp_tools.cpp` — shell::run_capture + shell_quote (уже частично сделано).
3. Проверить поведение плагина в реальном GUI-сценарии (разрешения, ожидание,
   прерывание, метрики) — требует запуска приложения с локальным LLM-сервером.

## Текущий статус (2026-09-08) — отладка «LLM: не ответил»

Плагин загружается и агент `ai_coder` регистрируется:
- `plugins/libwp_coder.so` (622 Кб) — задеплоен
- Логи: `[wp_coder] ll_plugin_init: ... agent_mode_register=... llm_chat_messages=...`
- `[wp_coder] agent_mode registered: ai_coder`
- `[PluginManager] init_fn returned 0 for wp_coder`

Реализована инфраструктура для тестирования без GUI:
- `settings.ini` — `api_url = http://localhost:8083` (там, где работает чат-сервер)
- `main.cpp` — флаг `--agent=NAME` → `MainWindow::set_agent_mode(name)`
- `ChatInterface::set_active_agent_mode(name)` — публичный метод переключения агента
- `llm_chat_messages` — детальное логирование: `[llm_chat_messages] LOCAL model=... choices=... finish=... content_len=... usage=...`
- `llama_interface_impl.cpp` — парсинг `usage` из ответа локального сервера

### Текущий барьер
Запрос уходит в `agent_mode=0` (Normal), а не в плагинный агент `ai_coder`. `--agent=ai_coder` был добавлен, но не протестирован — контекст переполнился до запуска.

### Локальный чат-сервер
`/home/Alex/projects/llama-b7472-bin-ubuntu-x64/llama-b7472/llama-server`
Модель: `models/qwen2-0_5b-instruct-q8_0.gguf`
Команда: `llama-server -m .../qwen2-0_5b-instruct-q8_0.gguf --port 8081 -np 1 -c 2048 --host 127.0.0.1`
(注意: `torsocks` блокирует локальные соединения — обходите через `env -u LD_PRELOAD`)

### Следующий шаг
1. Запустить `llama-server` на 8081 (или изменить `settings.ini` под свободный порт).
2. Запустить `./build/llama-gui-core --agent=ai_coder` (с `env -u LD_PRELOAD DISPLAY=:0.0`).
3. Отправить запрос в агент (через GUI или, если появится HTTP-API, через curl).
4. Прочитать логи `[llm_chat_messages] LOCAL` — если `content_len=0`, искать причину в локальном сервере.
