# Разработка модуля для AI-кодера

Гайд по созданию нового модуля для AI-кодера.

## Структура модуля

```
modules/<module_name>/
├── module.json              # Манифест модуля
├── <module_name>_module.h   # Заголовок (регистрация)
├── <module_name>_module.cpp # Реализация (точка входа)
├── <module_name>_tools.h    # Инструменты (заголовок)
├── <module_name>_tools.cpp  # Инструменты (реализация)
├── <module_name>_prompts.h  # Системный промпт
└── skills/                  # Навыки (.md файлы)
```

## Шаг 1: Манифест module.json

```json
{
    "name": "my_module",
    "version": "0.1.0",
    "description": "Описание модуля",
    "author": "your_name",
    "type": "module",
    "parent_plugin": "wp_coder",
    "capabilities": ["capability1", "capability2"],
    "tools": ["tool1", "tool2"],
    "skills": ["skill1", "skill2"]
}
```

## Шаг 2: Заголовок модуля

```cpp
// modules/my_module/my_module.h
#pragma once
#include "../../core/module_api.h"

namespace coder {
namespace my_module {

void register_module();

} // namespace my_module
} // namespace coder
```

## Шаг 3: Реализация модуля

```cpp
// modules/my_module/my_module.cpp
#include "my_module.h"
#include "my_tools.h"
#include "my_prompts.h"

namespace coder {
namespace my_module {

namespace {

void my_init() {
    register_my_tools();
}

std::vector<ToolInfo> my_get_tools() { return {}; }

std::vector<Skill> my_get_skills() {
    return {{"my_skill", "Описание навыка", "Тело навыка"}};
}

const char* my_get_system_prompt() {
    return "## МОДУЛЬ: MY MODULE\n\nОписание модуля...";
}

void my_render_panel() {}
void my_load_settings(const std::string&) {}
void my_save_settings() {}
void my_shutdown() {}

} // anonymous namespace

static const CoderModule s_module = {
    "my_module",
    "My Module",
    "Описание для UI",
    my_init,
    my_get_tools,
    my_get_skills,
    my_get_system_prompt,
    my_render_panel,
    my_load_settings,
    my_save_settings,
    my_shutdown
};

void register_module() {
    ModuleRegistry::instance().register_module(&s_module);
}

} // namespace my_module
} // namespace coder
```

## Шаг 4: Инструменты

```cpp
// modules/my_module/my_tools.h
#pragma once
#include "../../core/module_api.h"
#include <vector>

namespace coder {
namespace my_module {

void register_my_tools();
std::vector<Skill> get_my_skills();

} // namespace my_module
} // namespace coder
```

```cpp
// modules/my_module/my_tools.cpp
#include "my_tools.h"
#include "../../core/tools_registry.h"
#include "../../core/engine.h"

namespace coder {
namespace my_module {

void register_my_tools() {
    auto& reg = ToolsRegistry::instance();

    reg.register_tool("my_tool", [](const ToolArgs& a) -> std::string {
        // a.path, a.query, a.content, a.cli, a.url — параметры от агента
        return "Результат: " + a.query;
    }, "Описание инструмента");
}

} // namespace my_module
} // namespace coder
```

## Шаг 5: Системный промпт

```cpp
// modules/my_module/my_prompts.h
#pragma once

namespace coder {
namespace my_module {

inline const char* kMySystemPrompt =
    "## МОДУЛЬ: MY MODULE\n\n"
    "Ты — специалист по ...\n\n"
    "### ДОСТУПНЫЕ ИНСТРУМЕНТЫ\n\n"
    "my_tool — описание    QUERY: <параметр>\n\n"
    "### ПРАВИЛА\n\n"
    "- Правило 1\n"
    "- Правило 2";

} // namespace my_module
} // namespace coder
```

## Шаг 6: Регистрация в plugin_main.cpp

```cpp
// В начале файла:
#include "modules/my_module/my_module.h"

// В ll_plugin_init():
coder::my_module::register_module();
```

## Шаг 7: Добавление в CMakeLists.txt

```cmake
set(MODULE_SOURCES
    ...
    modules/my_module/my_module.cpp
    modules/my_module/my_tools.cpp
)
```

## Интерфейс ToolArgs

```cpp
struct ToolArgs {
    std::string path;      // PATH
    std::string root;      // ROOT
    std::string query;     // QUERY
    std::string pattern;   // PATTERN
    std::string content;   // CONTENT (между CONTENT_BEGIN/END)
    std::string cli;       // CLI
    std::string url;       // URL
    int k = 6;             // K
};
```

## Безопасность

Используй `security.h` для проверки путей:

```cpp
#include "../../core/security.h"

if (!security::is_path_safe(a.path))
    return "[запрещено] небезопасный путь";
if (!security::is_path_not_dangerous(abs_path))
    return "[запрещено] запись запрещена";
```

## Навыки (Skills)

Создай .md файл в `modules/<module>/skills/`:

```markdown
# skill_name
Описание: краткое описание
Тело навыка — инструкция для LLM.
```

Навыки автоматически загружаются из каталога `skills/`.
