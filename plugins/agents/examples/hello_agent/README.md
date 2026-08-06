# Hello Agent Plugin

Простейший пример плагина для системы агентов llama-gui.

## Описание

Этот плагин демонстрирует:
- Минимальную реализацию агента через интерфейс `IAgent`
- Экспорт функций через C-API (`plugin_c_api.h`)
- Обработку запросов с параметрами
- Логирование через `AgentContext`

## Сборка

### Отдельная сборка

```bash
cd plugins/examples/hello_agent
mkdir build && cd build
cmake ..
make
```

Плагин будет создан как `libhello_agent_plugin.so` (Linux) или `hello_agent_plugin.dll` (Windows).

### Сборка в составе проекта

```bash
cd ../../../../build
cmake -DBUILD_HELLO_AGENT=ON ..
make
```

## Установка

Скопируйте файл плагина в директорию плагинов:

```bash
# Linux
cp libhello_agent_plugin.so ../../../plugins/

# Windows
copy hello_agent_plugin.dll ..\..\..\plugins\
```

## Использование

### Через код

```cpp
#include <agents/agents.h>

using namespace agents;

// Загружаем плагин
PluginLoader loader;
AgentRegistry registry;

loader.load_plugin("plugins/libhello_agent_plugin.so");
loader.create_agent_from_plugin("hello_agent", &registry);

// Инициализируем
AgentContext context;
registry.initialize_all(&context);

// Выполняем запрос
AgentRequest request("hello_agent", "greet");
request.with_param("name", "Alex");

AgentResult result = registry.execute(request);
if (result.is_ok()) {
    std::string greeting = result.get<std::string>("greeting");
    std::cout << greeting << std::endl;  // Hello, Alex!
}
```

### Команды (действия)

| Действие | Параметры | Описание | Пример результата |
|----------|-----------|----------|-------------------|
| `greet` | `name` (string), `count` (int) | Приветствие | `{"greeting": "Hello, Alex!"}` |
| `echo` | любые | Возвращает параметры | `{"key": "value"}` |
| `info` | - | Информация об агенте | `{"name": "hello_agent", ...}` |
| `add` | `a` (int), `b` (int) | Сложение чисел | `{"result": 5, "expression": "2 + 3"}` |

### Примеры запросов

#### Приветствие

```cpp
AgentRequest request("hello_agent", "greet");
request.with_param("name", "World");
// Результат: {"greeting": "Hello, World!", "name": "World", "count": 1}
```

#### Повторяющееся приветствие

```cpp
AgentRequest request("hello_agent", "greet");
request.with_param("name", "Agent");
request.with_param("count", 3);
// Результат: {"greeting": "Hello, Agent! (repeated 3 times)", ...}
```

#### Эхо

```cpp
AgentRequest request("hello_agent", "echo");
request.with_param("message", "Test");
request.with_param("value", 42);
// Результат: {"message": "Test", "value": 42}
```

#### Сложение

```cpp
AgentRequest request("hello_agent", "add");
request.with_param("a", 10);
request.with_param("b", 32);
// Результат: {"result": 42, "expression": "10 + 32"}
```

#### Информация

```cpp
AgentRequest request("hello_agent", "info");
// Результат: {
//   "name": "hello_agent",
//   "description": "Простой демонстрационный агент...",
//   "version": "1.0.0",
//   "api_version": "1.0.0"
// }
```

## Структура плагина

```
hello_agent/
├── hello_agent.cpp      # Реализация агента
├── CMakeLists.txt       # Конфигурация сборки
└── README.md            # Эта документация
```

## Создание своего плагина

1. Скопируйте эту директорию:
   ```bash
   cp -r hello_agent my_agent
   ```

2. Отредактируйте `hello_agent.cpp`:
   - Измените `name()`, `description()`, `version()`
   - Реализуйте свою логику в `execute()`
   - Обновите `capabilities()` если нужны права доступа

3. Обновите `CMakeLists.txt`:
   - Измените имя проекта
   - Измените имя библиотеки

4. Соберите плагин

## Отладка

Для отладки плагина:

1. Включите логирование в агенте:
   ```cpp
   context_->debug("hello_agent", "Debug message");
   context_->info("hello_agent", "Info message");
   ```

2. Запустите приложение с логом

3. Проверьте логи на наличие сообщений от агента

## Возможные расширения

Этот агент можно расширить:

- **HTTP запросы**: Добавьте `AgentCapability::HTTP_GET`
- **Работа с файлами**: Добавьте `AgentCapability::FILE_READ`
- **Взаимодействие**: Вызывайте другие агенты через `context_->call_agent()`

## См. также

- [AGENT_API.md](../../../docs/AGENT_API.md) - Полная документация API
- [plugin_c_api.h](../../../include/agents/plugin_c_api.h) - C-API для плагинов
- [i_agent.h](../../../include/agents/i_agent.h) - Базовый интерфейс агента
