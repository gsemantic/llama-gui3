# План: Система агентов (инструментов) для ядра

**Дата создания:** 2026-09-01 (восстановлено из сессии)
**Последнее обновление:** 2026-09-01
**Статус:** ✅ 100% выполнено + безопасность

---

## Цель

Вынести инструменты из плагина `wp_coder` в ядро приложения, создав универсальную систему агентов с единой архитектурой (IAgent interface), регистрацией через AgentRegistry и командами в чате.

---

## Фазы

### Фаза 0: Базовая инфраструктура (✅ выполнено ранее)
- `IAgent` интерфейс, `AgentContext`, `AgentRegistry`, `AgentRequest`, `AgentResult`
- `AgentCapability` flags, `SecurityManager`, `Sandbox`
- `AgentLogger`, `AgentConfig`

### Фаза 1: Миграция core-агентов из плагинов (✅ выполнено)
Перенос из `plugins/agents/official/` в `src/agents/`:

| Агент | Файл | Строк | Описание |
|---|---|---|---|
| file_agent | file_agent.cpp | 500+ | read/write/copy/move/list/delete/info файлов |
| web_search_agent | web_search_agent.cpp | 512 | HTTP-запросы, GET/POST, поиск |
| terminal_agent | terminal_agent.cpp | 361 | Выполнение команд shell (песочница) |
| rag_agent | rag_agent.cpp | 242 | Поиск по проиндексированным документам |
| code_agent | code_agent.cpp | 523 | Генерация/анализ кода (через LLM) |
| summarization_agent | summarization_agent.cpp | 440 | Суммаризация текста (через LLM) |
| web_render_agent | web_render_agent.cpp | 165 | Рендер DOM через headless Chromium |

### Фаза 2: Новые инструменты (✅ выполнено)
Созданы с нуля:

| Агент | Файл | Строк | Описание |
|---|---|---|---|
| edit_agent | edit_agent.cpp | ~150 | Точечная замена текста в файле (old→new) |
| glob_agent | glob_agent.cpp | 166 | Поиск файлов по паттерну (glob) |
| grep_agent | grep_agent.cpp | ~200 | Поиск содержимого по regex |

### Фаза 3: Управление задачами и взаимодействие (✅ выполнено)

| Агент | Файл | Строк | Описание |
|---|---|---|---|
| todowrite_agent | todowrite_agent.cpp | ~120 | Список задач с статусами (pending/in_progress/completed/cancelled) |
| question_agent | question_agent.cpp | 75 | Запрос ответа пользователя через callback |

### Фаза 4: Интеграция в ядро (✅ выполнено)
- Удалён `PluginLoader` (динамическая загрузка .so агентов)
- Агенты компилируются статически в бинарник
- `agent_context_.set_llm_complete()` — callback для вызова LLM из агентов
- Регистрация 12 агентов в `MainWindow::initialize_agent_system()`

### Фаза 5: Команды чата (✅ выполнено)
Добавлены в `agent_commands.cpp`:

| Команда | Агент | Описание |
|---|---|---|
| `/file <action> <path>` | file_agent | Файловые операции |
| `/edit <file> <old> <new>` | edit_agent | Замена текста |
| `/glob <pattern> [path]` | glob_agent | Поиск файлов |
| `/grep <pattern> [path] [include]` | grep_agent | Поиск в содержимом |
| `/code <prompt>` | code_agent | Генерация кода |
| `/terminal <cmd>` | terminal_agent | Выполнение shell-команды |
| `/rag <query>` | rag_agent | RAG-поиск |
| `/search <query>` | web_search_agent | Веб-поиск |
| `/summarize <text>` | summarization_agent | Суммаризация |
| `/todo [set\|get\|update\|clear]` | todowrite_agent | Управление задачами |
| `/question <text>` | question_agent | Вопрос пользователю |
| `/agents` | — | Список всех агентов |

### Фаза 6: LLM-интеграция для агентов (✅ выполнено)
- `code_agent` и `summarization_agent` вызывают LLM через `context->llm_complete()`
- Callback настроен в `main_window.cpp` через `llama_interface_.create_chat_completion_async()`
- Таймаут 120 сек, temperature 0.3, max_tokens 2048

### Фаза 7: Безопасность — ограничение по проекту (✅ выполнено)
- `AgentContext::set_project_root(dir)` — установка корня проекта
- `AgentContext::is_within_project(path)` — проверка пути
- Валидация в `file_agent`, `edit_agent`, `glob_agent`, `grep_agent`
- При попытке доступа за пределы проекта — ошибка `Access denied`
- `project_root` = cwd при запуске приложения

### Фаза 8: Перехват команд в облачном режиме (✅ выполнено)
- Добавлен перехват `/`-команд в `send_message_via_openrouter()`
- Команды работают в обоих режимах (локальный сервер и облако)

---

## Удалённый код

| Файл | Причина |
|---|---|
| `src/agents/plugin_loader.cpp` | Заменён статической регистрацией |
| `include/agents/plugin_loader.h` | Заменён статической регистрацией |
| `include/agents/plugin_c_api.h` | Заменён статической регистрацией |

---

## Структура файлов

```
include/agents/
├── agents.h              # Базовый интерфейс IAgent
├── agent_context.h       # Контекст выполнения (+ LlmCompleteFn, project_root)
├── agent_registry.h      # Реестр агентов
├── agent_capabilities.h  # Флаги возможностей
├── security_manager.h    # Безопасность
├── sandbox.h             # Песочница
├── agent_logger.h        # Логирование
├── agent_config.h        # Конфигурация
├── file_agent.h          # ✅ Файлы (+ проверка project_root)
├── edit_agent.h          # ✅ Замена текста (+ проверка project_root)
├── glob_agent.h          # ✅ Glob-поиск (+ проверка project_root)
├── grep_agent.h          # ✅ Содержимое (+ проверка project_root)
├── code_agent.h          # ✅ Код (LLM)
├── terminal_agent.h      # ✅ Терминал
├── rag_agent.h           # ✅ RAG
├── web_search_agent.h    # ✅ Веб-поиск
├── web_render_agent.h    # ✅ Рендер
├── summarization_agent.h # ✅ Суммаризация (LLM)
├── todowrite_agent.h     # ✅ Задачи
└── question_agent.h      # ✅ Вопрос пользователю

src/agents/
├── agent_context.cpp     # + llm_complete, + project_root, + is_within_project
├── agent_registry.cpp
├── security_manager.cpp
├── sandbox.cpp
├── agent_logger.cpp
├── agent_config.cpp
├── file_agent.cpp        # ~500 стр. (+ check_project_root)
├── edit_agent.cpp        # ~150 стр. (+ project root check)
├── glob_agent.cpp        # 166 стр. (+ project root check)
├── grep_agent.cpp        # ~200 стр. (+ project root check)
├── code_agent.cpp        # 523 стр.
├── terminal_agent.cpp    # 361 стр.
├── rag_agent.cpp         # 242 стр.
├── web_search_agent.cpp  # 512 стр.
├── web_render_agent.cpp  # 165 стр.
├── summarization_agent.cpp # 440 стр.
├── todowrite_agent.cpp   # ~120 стр.
└── question_agent.cpp    # 75 стр.
```

---

## Метрики

| Метрика | Значение |
|---|---|
| Всего агентов | 12 |
| Зарегистрировано в ядре | 12 (все) |
| Команд чата | 12 |
| Суммарно строк кода | ~5,600 |
| Сборка | ✅ Проходит |
| Мёртвый код | ✅ Удалён (plugin_loader) |
| Безопасность | ✅ project_root restriction |
| Облачный режим | ✅ Команды перехватываются |

---

## Ограничения и безопасность

### Project Root Restriction
Файловые агенты (`file`, `edit`, `glob`, `grep`) проверяют, что целевой путь находится внутри `project_root` (cwd при запуске). При попытке доступа за пределы — ошибка:

```
Access denied: path is outside the project directory.
Path: /etc/passwd
Project root: /home/user/myproject
```

### Sandbox (терминал)
`terminal_agent` использует `Sandbox` с ограничениями:
- Таймаут 30 сек
- Ограничение памяти 512 MB
- Ограничение вывода 1024 KB

### Режим Research
В режиме Research (режим 1 в wp_coder) мутирующие инструменты (`write_file`, `deploy`) запрещены.

---

## Текущие ограничения / бэклог

1. **question_agent** — callback пока echo (нет блокирующего UI-диалога)
2. **terminal_agent** — нет проверки project_root (полагается на Sandbox)
3. **Тесты** — нет unit-тестов для новых агентов (edit, glob, grep, todowrite, question)
4. **UI-виджет** — todowrite/question только в чате, нет отдельной панели
