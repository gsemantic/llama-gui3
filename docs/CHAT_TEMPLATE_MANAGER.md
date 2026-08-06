# Chat Template Manager

Модуль для автоматического управления chat templates в llama-gui-core.

## Обзор

`ChatTemplateManager` предоставляет централизованное управление шаблонами чата для различных моделей LLM:

- **Авто-определение** из GGUF файла модели
- **Встроенные шаблоны** для популярных моделей
- **Загрузка из файлов** (.jinja формат)
- **Кэширование** результатов для производительности

## Быстрый старт

### Использование в профилях

В файле профиля (`profiles/*.json`) укажите `chat_template`:

```json
{
    "grammar": {
        "chat_template": "auto"
    }
}
```

### Специальные значения

| Значение | Описание |
|----------|----------|
| `"auto"` | Автоматически извлечь из GGUF файла модели |
| `"none"` | Отключить применение шаблона |
| `"file:/path/to/template.jinja"` | Загрузить из файла |
| `"llama-3"`, `"mistral"`, etc. | Использовать встроенный шаблон |
| `{{...}}` (содержит "{{") | Inline-шаблон (Jinja) |

## API

### Singleton доступ

```cpp
#include "chat_template_manager.h"

auto& manager = llama_gui::core::ChatTemplateManager::instance();
```

### Извлечение из GGUF

```cpp
auto result = manager.extract_from_gguf("/path/to/model.gguf");

if (result.success) {
    std::cout << "Template: " << result.template_str << std::endl;
    std::cout << "Source: " << result.source << std::endl;  // "gguf"
} else {
    std::cerr << "Error: " << result.error_message << std::endl;
}
```

### Встроенные шаблоны

```cpp
auto result = manager.get_builtin_template("llama-3");

if (result.success) {
    // Использовать result.template_str
}
```

### Загрузка из файла

```cpp
auto result = manager.load_from_file("profiles/chat_templates/llama-3.jinja");
```

### Универсальный метод

```cpp
// mode может быть: "auto", "none", "file:/path", "llama-3", inline-шаблон
auto result = manager.get_or_extract_template("/path/to/model.gguf", mode);
```

## Структура результата

```cpp
struct ChatTemplateResult {
    std::string template_str;    // Шаблон
    std::string source;          // "gguf", "file", "builtin", "inline", "none"
    std::string model_name;      // Имя модели (если из GGUF)
    bool success = false;
    std::string error_message;
};
```

## Встроенные шаблоны

Доступные встроенные шаблоны:

| Название | Модели |
|----------|--------|
| `llama-2` | LLaMA 2, Alpaca |
| `llama-3` | LLaMA 3 |
| `llama-3.1` | LLaMA 3.1 |
| `mistral` | Mistral Base |
| `mistral-instruct` | Mistral Instruct |
| `qwen-2` | Qwen 2, Qwen 2.5 |
| `gemma` | Gemma, Gemma 2 |
| `phi-3` | Phi-3, Phi-2 |
| `chatglm3` | ChatGLM3, GLM-Edge |
| `yi` | Yi, Yi-Lightning |
| `openchat` | OpenChat |
| `vicuna` | Vicuna |

Получить список названий:

```cpp
auto names = manager.get_builtin_template_names();
```

## Файлы шаблонов

Шаблоны хранятся в `profiles/chat_templates/`:

```
profiles/chat_templates/
├── llama-2.jinja
├── llama-3.jinja
├── mistral-instruct.jinja
├── qwen-2.jinja
├── gemma.jinja
├── phi-3.jinja
└── chatglm3.jinja
```

### Пример использования файла в профиле

```json
{
    "grammar": {
        "chat_template": "file:profiles/chat_templates/llama-3.jinja"
    }
}
```

## Интеграция с ModelManager

При загрузке модели `ModelManager` автоматически извлекает chat template:

```cpp
// В model_manager.cpp
bool ModelManager::load_model(const std::string& model_path) {
    // ... загрузка модели ...
    
    extract_chat_template_if_needed(model_path);
    
    return true;
}
```

Логирование:

```
[ModelManager] Extracting chat template from GGUF: /path/to/model.gguf
[ModelManager] Chat template extracted: 1137 bytes
[ModelManager] Template source: gguf
[ModelManager] Model name: Llama-3-8B-Instruct
```

## Кэширование

`ChatTemplateManager` кэширует результаты:

- **GGUF кэш**: по пути к файлу модели
- **Файловый кэш**: по пути к файлу шаблона

Очистка кэша:

```cpp
manager.clear_cache();
```

## Валидация и helper методы

В `GrammarSettings` доступны методы проверки:

```cpp
GrammarSettings settings;

settings.is_chat_template_auto();      // true если "auto"
settings.is_chat_template_none();      // true если "none"
settings.is_chat_template_file_path(); // true если путь к файлу
settings.get_chat_template_file_path(); // Получить путь
settings.is_chat_template_builtin();   // true если встроенный
settings.is_chat_template_inline();    // true если inline-шаблон
```

## Примеры использования

### Пример 1: Авто-определение

```cpp
GrammarSettings settings;
settings.chat_template = "auto";  // Значение по умолчанию

// При загрузке модели шаблон будет извлечён автоматически
```

### Пример 2: Принудительный встроенный шаблон

```cpp
GrammarSettings settings;
settings.chat_template = "llama-3";  // Использовать Llama 3 шаблон
```

### Пример 3: Загрузка из файла

```cpp
GrammarSettings settings;
settings.chat_template = "file:/path/to/custom.jinja";
```

### Пример 4: Inline-шаблон

```cpp
GrammarSettings settings;
settings.chat_template = "{% for message in messages %}{{ message['content'] }}{% endfor %}";
```

### Пример 5: Отключение шаблона

```cpp
GrammarSettings settings;
settings.chat_template = "none";  // Без шаблона
```

## Обработка ошибок

```cpp
auto result = manager.extract_from_gguf("/nonexistent.gguf");

if (!result.success) {
    std::cerr << "Failed: " << result.error_message << std::endl;
    // Fallback к встроенному шаблону или шаблону по умолчанию
}
```

## Потокобезопасность

`ChatTemplateManager` потокобезопасен:

- Все публичные методы защищены мьютексом
- Кэш синхронизирован
- Singleton инициализируется thread-safe

## Расширение

### Добавление встроенного шаблона

Отредактируйте `get_builtin_templates_map()` в `chat_template_manager.cpp`:

```cpp
static const std::unordered_map<std::string, std::string> templates = {
    // ... существующие шаблоны ...
    {"my-custom", "{% for message in messages %}..."},
};
```

### Добавление эвристики определения

Отредактируйте `detect_from_model_name()`:

```cpp
if (lower_name.find("my-model") != std::string::npos) {
    return "my-custom";
}
```

## Отладка

Включите логирование:

```cpp
// В settings или через环境变量
export LLAMA_GUI_LOG_LEVEL=debug
```

Пример вывода:

```
[ChatTemplate] Extracting from: /models/llama-3.gguf
[ChatTemplate] Found template: 1137 bytes
[ChatTemplate] Source: gguf
[ChatTemplate] Model: Llama-3-8B-Instruct
```

## См. также

- [llama.cpp Chat Templates](https://github.com/ggml-org/llama.cpp/wiki/Templates-supported-by-llama_chat_apply_template)
- [HuggingFace Chat Templates](https://huggingface.co/docs/transformers/main/en/chat_templating)
- [Jinja Template Documentation](https://jinja.palletsprojects.com/)
