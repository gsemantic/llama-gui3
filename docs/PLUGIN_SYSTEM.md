# Система плагинов llama-gui

## Обзор

Система плагинов позволяет расширять функциональность llama-gui без изменения кода приложения. Плагины — это разделяемые библиотеки (`.so`/`.dll`/`.dylib`), которые:

- разрабатываются **полностью независимо** — подключают только SDK-заголовок `include/plugins/plugin_api.h`;
- получают доступ к возможностям приложения через таблицу функций хоста `LlamaHostApi`;
- рисуют свои окна через Dear ImGui (символы резолвятся из исполняемого файла приложения);
- органично встраиваются в UI приложения: меню, команды, хоткеи, окна, диалоги, настройки.

```
┌─────────────────────────────────────────────┐
│              llama-gui (приложение)          │
│                                             │
│  PluginManager ──► загружает .so из plugins/│
│       │                                     │
│       ▼                                     │
│  LlamaHostApi (таблица функций хоста)       │
│   ├── меню / команды / хоткеи               │
│   ├── окна / диалоги                        │
│   ├── настройки / состояние                 │
│   ├── чат / LLM                             │
│   ├── RAG                                   │
│   └── пути                                  │
│       │                                     │
│       ▼                                     │
│  Плагин (hello_plugin.so)                   │
│   ├── ll_plugin_init / render / shutdown    │
│   └── Dear ImGui (из exe)                   │
└─────────────────────────────────────────────┘
```

## Как это работает

1. При запуске `MainWindow::initializePlugins()` заполняет `PluginSubsystems` (срезы подсистем приложения) и вызывает `PluginManager::initialize()`.
2. `PluginManager` ищет директорию плагинов (заданная, `plugins/` в cwd, `plugins/` рядом с exe) и сканирует `*.so`/`*.dll`.
3. Каждая библиотека загружается через `dlopen`. Проверяется экспорт обязательных функций и совпадение версии API (`ll_plugin_api_version`).
4. Вызывается `ll_plugin_init(host, api)`: плагин регистрирует команды, меню, окна.
5. Каждый кадр приложение вызывает `ll_plugin_render()` — плагин рисует окна.
6. При выходе `PluginManager::shutdown()` вызывает `ll_plugin_shutdown()` и выгружает библиотеку, предварительно удалив команды плагина.

## Версионирование API

- `LLAMA_PLUGIN_API_VERSION` — текущая версия (`"1.0.0"`).
- Хост сравнивает версию плагина с версией хоста и отказывается загружать несовместимые плагины.
- Плагин, возвращающий другую версию, не будет загружен (лог в stderr).

## Структура плагина

### Обязательные экспортируемые функции

```c
/* Версия API, с которой совместим плагин */
LLAMA_PLUGIN_EXPORT const char* ll_plugin_api_version(void);

/* Статическая информация о плагине */
LLAMA_PLUGIN_EXPORT const LlamaPluginInfo* ll_plugin_info(void);

/* Инициализация: регистрация команд, меню, окон. 0 = успех. */
LLAMA_PLUGIN_EXPORT int ll_plugin_init(LlamaPluginHost* host, const LlamaHostApi* api);
```

### Опциональные функции

```c
/* Вызывается каждый кадр внутри активного ImGui-контекста */
LLAMA_PLUGIN_EXPORT void ll_plugin_render(void);

/* Освобождение ресурсов перед выгрузкой */
LLAMA_PLUGIN_EXPORT void ll_plugin_shutdown(void);
```

### Пример (plugins/examples/hello_plugin)

```c
#include "plugins/plugin_api.h"
#include "imgui.h"

static LlamaPluginHost* g_host = nullptr;
static const LlamaHostApi* g_api = nullptr;

extern "C" {

LLAMA_PLUGIN_EXPORT const char* ll_plugin_api_version(void) {
    return LLAMA_PLUGIN_API_VERSION;
}

LLAMA_PLUGIN_EXPORT const LlamaPluginInfo* ll_plugin_info(void) {
    static const LlamaPluginInfo info = {
        "hello_plugin", "1.0.0",
        "Пример: меню, окно, команды, диалоги, чат, RAG",
        "llama-gui"
    };
    return &info;
}

LLAMA_PLUGIN_EXPORT int ll_plugin_init(LlamaPluginHost* host, const LlamaHostApi* api) {
    g_host = host;
    g_api = api;

    // Команда + хоткей
    api->command_register(host, "hello_plugin_open_window",
                          cmd_open_window, nullptr,
                          "Open Hello Plugin window", "Ctrl+Shift+H");

    // Меню
    LlamaPluginMenu* menu = api->menu_add(host, "Hello Plugin");
    api->menu_add_item(host, menu, "Open Window",
                       "hello_plugin_open_window", "Ctrl+Shift+H");

    // Окно (рисуется в ll_plugin_render)
    g_window = api->window_register(host, "hello_plugin", "Hello Plugin");
    return 0;
}

LLAMA_PLUGIN_EXPORT void ll_plugin_render(void) {
    if (!g_api->window_is_visible(g_host, g_window)) return;
    ImGui::Begin("Hello Plugin");
    ImGui::Text("Привет из плагина!");
    ImGui::End();
}

} // extern "C"
```

## Таблица функций хоста `LlamaHostApi`

Все строки, возвращаемые как `char*` (`settings_get`, `state_get`, `rag_search`, `rag_build_prompt`, `llm_complete`), выделяются хостом и **обязательно** освобождаются плагином через `free_string()`. Массивы `float` — через `free_float_array()`.

| Группа | Функции | Описание |
|---|---|---|
| Логирование | `log` | Вывод в stderr с префиксом `[Plugin:<имя>]` |
| Меню | `menu_add`, `menu_add_item`, `menu_add_separator` | Построение меню приложения |
| Команды | `command_register`, `command_execute` | Команды + горячие клавиши |
| Окна | `window_register`, `window_set_visible`, `window_is_visible` | Видимость управляется WindowManager приложения |
| Диалоги | `dialog_info`, `dialog_warning`, `dialog_error`, `dialog_confirmation` | Модальные диалоги |
| Настройки | `settings_get`, `settings_set` | Persist в settings.ini, значения — JSON-строки |
| Состояние | `state_get`, `state_set` | In-memory хранилище плагинов |
| Чат / LLM | `chat_send_message`, `chat_add_message`, `llm_is_connected`, `llm_complete` | Отправка сообщений, блокирующее завершение |
| RAG | `rag_search`, `rag_process_document`, `rag_embedding`, `rag_index_count`, `rag_build_prompt` | Гибридный поиск, индексация, эмбеддинги |
| Пути | `path_config_dir`, `path_data_dir`, `path_plugins_dir` | Директории приложения |
| Освобождение | `free_string`, `free_float_array` | Освобождение памяти хоста |

### Чат / LLM

- `chat_send_message(message)` — отправляет сообщение так, как если бы его ввёл пользователь (проходит полный конвейер `send_message`, включая проверку загрузки модели).
- `chat_add_message(role, content)` — добавляет сообщение напрямую в историю без запуска конвейера. `role`: `"user"` или `"assistant"`.
- `llm_complete(prompt, &out)` — **блокирующий** вызов; возвращает 1 при успехе и заполняет `*out` строкой (освободить через `free_string`). Если локальный сервер недоступен, хост автоматически отправляет запрос подключённому облачному провайдеру (настройки `cloud_provider` + API-ключ из `.env`), а при неудаче возвращает 0.

### RAG

- `rag_search(query, k, path_filter)` — возвращает JSON-массив чанков:
  `[{"content", "document_id", "chunk_index", "file_path", "symbol_name", "start_line", "end_line"}]`.
- `rag_process_document(path)` — индексирует документ (1 = успех).
- `rag_embedding(text, vec, max_dim, &dim)` — вектор эмбеддинга текста.
- `rag_index_count()` — количество внешних чанков в индексе.
- `rag_build_prompt(query, k, path_filter)` — собирает промпт с контекстом RAG.

## Манифест plugin.json

Каждый плагин может содержать файл `plugin.json` рядом с библиотекой (`<имя>.json`, `<имя>.plugin.json` или `plugin.json` в той же папке). Хост читает его при загрузке:

- проверяет согласованность `name` и `api_version` с данными из `ll_plugin_info` (при расхождении — предупреждение);
- сохраняет метаданные (`name`, `version`, `description`, `author`, `api_version`, `permissions`, `capabilities`) в `PluginInfo::manifest` — доступны через `PluginManager::list_plugins()`.

**Пермиссии на текущем этапе информационные и не применяются** — загрузка плагина не блокируется отсутствием пермиссии. Схема манифеста: `plugins/plugin.schema.json`.

```json
{
  "name": "hello_plugin",
  "version": "1.0.0",
  "description": "Пример плагина",
  "author": "llama-gui",
  "api_version": "1.0.0",
  "permissions": ["rag_access", "chat_access"],
  "capabilities": ["demo", "ui"]
}
```

## Сборка плагина

Используйте `plugins/examples/hello_plugin/CMakeLists.txt` как шаблон:

```cmake
cmake_minimum_required(VERSION 3.14)
project(hello_plugin CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

add_library(hello_plugin SHARED hello_plugin.cpp)

target_include_directories(hello_plugin PRIVATE
    ${PROJECT_ROOT}/include
    ${PROJECT_ROOT}/external/imgui
    ${PROJECT_ROOT}/external/imgui/backends
)

# Куда положить .so (приложение ищет плагины в build/plugins/)
set_target_properties(hello_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
)
```

Требования:

- Плагин подключает только `include/plugins/plugin_api.h` и `imgui.h`. **Не подключайте** внутренние заголовки приложения (`include/ui/*.h`, `include/core/*.h`) — это нарушает изоляцию.
- ImGui-символы резолвятся из исполняемого файла (флаг `-rdynamic` в корневом CMakeLists), поэтому библиотеки ImGui линковать не нужно.
- `.so` кладётся в `build/plugins/` (или другую директорию, заданную в `PluginSubsystems.plugins_dir`).

## Установка плагина

1. Собрать плагин как `.so`.
2. Положить в директорию плагинов (`plugins/` рядом с exe или cwd).
3. Запустить приложение — плагин загрузится автоматически (см. лог `[PluginManager] Loaded plugin: ...`).

## Добавление нового плагина в репозиторий

1. Создать `plugins/examples/<имя>/` с `CMakeLists.txt`, `<имя>.cpp`, `plugin.json`.
2. Добавить `add_subdirectory(<имя>)` в `plugins/examples/CMakeLists.txt`.
3. Собрать: плагин появится в `build/plugins/lib<имя>.so`.

## Тестирование

```bash
# Интеграционный тест загрузки/выгрузки плагина через реальный PluginManager
./build/tests/test_plugin_loader

# Весь набор тестов
cmake --build build && ctest --test-dir build --output-on-failure
```

## Примечания и ограничения

- `llm_complete` — блокирующий вызов, не используйте его в колбэках отрисовки ImGui (риск зависания UI).
- `hello_plugin` регистрирует команду с хоткеем `Ctrl+Shift+H` — при конфликте с существующей командой регистрация пропускается с предупреждением.
- Плагин, возвращающий несовместимую версию API, не загружается.
- Старая agent-система перемещена в `plugins/agents/` и не собирается по умолчанию (см. `BUILD_AGENT_PLUGINS`).

## Ключевые файлы

| Файл | Назначение |
|---|---|
| `include/plugins/plugin_api.h` | SDK плагина (C ABI, таблица хоста) |
| `include/plugins/plugin_manager.h` | Хост: `PluginManager`, `PluginSubsystems`, `PluginInfo` |
| `src/plugins/plugin_manager.cpp` | Реализация хоста, таблица API, загрузка/выгрузка |
| `src/ui/main_window.cpp` | Интеграция: `initializePlugins()`, `render_plugins()` |
| `plugins/examples/hello_plugin/` | Рабочий пример плагина |
| `plugins/examples/calculator/` | Пример плагина с окном, командой и LLM |
| `tests/core/test_plugin_loader.cpp` | Интеграционный тест загрузки плагина |
