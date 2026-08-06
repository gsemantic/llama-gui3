# Шаблон плагина для системы агентов llama-gui

Этот шаблон предназначен для быстрого создания новых агентов.

## Быстрый старт

### 1. Скопируйте шаблон

```bash
cd plugins/user_plugins/
cp -r ../examples/template_agent my_agent
cd my_agent
```

### 2. Переименуйте файлы

```bash
mv template_agent.h my_agent.h
mv template_agent.cpp my_agent.cpp
```

### 3. Отредактируйте код

**my_agent.h:**
```cpp
class MyAgent : public IAgent {  // Было: TemplateAgent
    // ...
};
```

**my_agent.cpp:**
```cpp
const char* TemplateAgent::name() const {
    return "my_agent";  // Было: "template_agent"
}
```

### 4. Обновите plugin.json

```json
{
    "name": "my_agent",
    "version": "1.0.0",
    "description": "Описание моего агента",
    "author": "Ваше имя",
    "api_version": "1.0.0",
    "library": "libmy_agent_plugin.so"
}
```

### 5. Обновите CMakeLists.txt

```cmake
project(my_agent_plugin CXX)
add_library(my_agent_plugin SHARED my_agent.cpp)
```

### 6. Соберите плагин

```bash
mkdir build && cd build
cmake ..
make
```

### 7. Установите плагин

```bash
cp libmy_agent_plugin.so ~/.llama-gui/plugins/
```

---

## Структура шаблона

```
template_agent/
├── template_agent.h      # Заголовок агента
├── template_agent.cpp    # Реализация + C-API
├── CMakeLists.txt        # Сборка
├── plugin.json.example   # Пример plugin.json
└── README.md             # Эта инструкция
```

---

## Добавление действий

### 1. Объявите обработчик в заголовке

**my_agent.h:**
```cpp
private:
    AgentResult handle_default(const AgentRequest& request);
    AgentResult handle_my_action(const AgentRequest& request);  // НОВОЕ
```

### 2. Реализуйте обработчик

**my_agent.cpp:**
```cpp
AgentResult TemplateAgent::handle_my_action(const AgentRequest& request) {
    std::string param = request.get_param<std::string>("param", "default");
    
    // Ваша логика
    // ...
    
    return AgentResult::success({{"result", "success"}});
}
```

### 3. Добавьте в execute()

**my_agent.cpp:**
```cpp
AgentResult TemplateAgent::execute(const AgentRequest& request) {
    std::string action = request.action();
    
    if (action == "default") {
        return handle_default(request);
    }
    else if (action == "my_action") {  // НОВОЕ
        return handle_my_action(request);
    }
    
    return AgentResult::error("Unknown action: " + action);
}
```

---

## Использование возможностей (Capabilities)

### В заголовке

```cpp
AgentCapability capabilities() const override {
    return AgentCapability::FILE_READ | AgentCapability::HTTP_GET;
}
```

### Доступные возможности

| Возможность | Описание |
|-------------|----------|
| `FILE_READ` | Чтение файлов |
| `FILE_WRITE` | Запись файлов |
| `HTTP_GET` | HTTP GET запросы |
| `HTTP_POST` | HTTP POST запросы |
| `RAG_SEARCH` | RAG поиск |
| `WEB_SEARCH` | Поиск в интернете |
| `CODE_GENERATION` | Генерация кода |
| `TERMINAL_EXEC` | Выполнение команд |
| `TEXT_SUMMARY` | Суммаризация текста |

---

## Логирование

```cpp
bool TemplateAgent::initialize(AgentContext* context) {
    context_ = context;
    
    // Разные уровни логирования
    context_->debug(name(), "Debug message");
    context_->info(name(), "Info message");
    context_->warning(name(), "Warning message");
    context_->error(name(), "Error message");
    
    return true;
}
```

---

## Проверка прав доступа

```cpp
AgentResult TemplateAgent::execute(const AgentRequest& request) {
    std::string file_path = request.file_path();
    
    // Проверка через SecurityManager
    if (context_) {
        auto security = context_->security();
        auto result = security->check_file_access(
            name(), file_path, false);
        
        if (!result.allowed) {
            return AgentResult::error("Access denied: " + result.reason);
        }
    }
    
    // ... выполнение действия
}
```

---

## Использование Sandbox

```cpp
#include <agents/sandbox.h>

AgentResult TemplateAgent::execute_command(const std::string& cmd) {
    SandboxConfig config;
    config.timeout_ms = 5000;
    config.max_memory_mb = 128;
    
    Sandbox sandbox(config);
    
    auto result = sandbox.run_with_timeout([&cmd]() {
        return std::system(cmd.c_str());
    }, config.timeout_ms);
    
    if (result.status == SandboxStatus::TIMEOUT) {
        return AgentResult::error("Timeout exceeded");
    }
    
    return AgentResult::success({
        {"exit_code", result.exit_code},
        {"output", result.output}
    });
}
```

---

## plugin.json

Пример файла метаданных:

```json
{
    "name": "my_agent",
    "version": "1.0.0",
    "description": "Мой первый агент",
    "author": "Ваше имя <email@example.com>",
    "api_version": "1.0.0",
    "library": "libmy_agent_plugin.so",
    "entry_point": "plugin_create_agent",
    "permissions": ["read_files"],
    "capabilities": ["custom_action"],
    "config": {
        "timeout_ms": 30000
    },
    "license": "MIT"
}
```

---

## Отладка

### 1. Включите логирование

**agents_config.json:**
```json
{
    "enable_logging": true,
    "log_level": "DEBUG",
    "log_file": "logs/agents.log"
}
```

### 2. Проверьте логи

```bash
tail -f logs/agents.log
```

### 3. Используйте отладчик

```bash
# GDB
gdb --args ./llama-gui-core
(gdb) break my_agent.cpp:50
(gdb) run
```

---

## Частые ошибки

### Ошибка: `Agent not found`

**Причина:** Плагин не загружен

**Решение:**
1. Проверьте, что .so файл в директории `plugins/`
2. Проверьте права доступа к файлу
3. Проверьте логи загрузки

### Ошибка: `Unknown action`

**Причина:** Действие не реализовано

**Решение:** Добавьте обработчик в `execute()`

### Ошибка компиляции: `undefined reference`

**Причина:** Не экспортированы функции

**Решение:** Проверьте `extern "C"` блок в конце .cpp файла

---

## Ссылки

- [Обзор системы агентов](../../../docs/AGENT_SYSTEM_OVERVIEW.md)
- [Руководство по плагинам](../../../docs/AGENT_PLUGIN_GUIDE.md)
- [API документация](../../../docs/AGENT_API.md)

---

**Шаблон является частью документации системы агентов llama-gui.**
