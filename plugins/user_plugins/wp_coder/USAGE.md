# wp-coder: Usage Guide

## Overview
`wp-coder` — это набор AI-агентов для автоматизации разработки на WordPress. 
Охватывает генерацию тем/плагинов, работу с хуками, деплой, RAG-поиск и безопасные файловые операции.

## Quick Start

### 1. Запуск оркестратора
```bash
# Создаем агент через фабрику
agents::IAgent* orchestrator = plugin_create_agent("wp_coder_orchestrator");
orchestrator->initialize(context);
```

### 2. Генерация темы WordPress
```json
{
  "action": "generate_theme",
  "params": {
    "description": "Современная блог-тема с поддержкой Gutenberg, темным режимом и SEO-оптимизацией"
  }
}
```
**Результат**: полный код темы с `style.css`, `index.php`, `functions.php`, `header.php`, `footer.php`.

### 3. Генерация плагина
```json
{
  "action": "generate_plugin",
  "params": {
    "description": "Плагин для шорткода [contact_form] с валидацией и отправкой на email"
  }
}
```
**Результат**: структура плагина с основным файлом, includes/, admin/, public/.

### 4. Поиск хуков
```json
{
  "action": "find_hooks",
  "params": {
    "query": "фильтр для изменения контента поста"
  }
}
```
**Результат**: список подходящих фильтров (`the_content`, `wp_insert_post_data` и др.).

### 5. Деплой на хостинг
```json
{
  "action": "deploy",
  "params": {
    "source": "/local/wp-site",
    "target": "/var/www/html",
    "wp_cli": "/usr/local/bin/wp"
  }
}
```
**Использует**: rsync + WP-CLI + LLM-генерация дополнительных команд.

### 6. RAG-поиск по кодовой базе
```json
{
  "action": "search_code",
  "params": {
    "query": "как правильно добавить meta box в WordPress",
    "k": 5
  }
}
```
**Результат**: семантически релевантные фрагменты из ядра WP, тем и плагинов.

### 7. Безопасные файловые операции
```json
{
  "action": "file_ops",
  "params": {
    "action": "read",
    "path": "wp-content/themes/my-theme/style.css"
  }
}
```
**Политики**: только wp-content, проверка расширений, лимит размера.

## Harness-профили

Выберите профиль в зависимости от задачи:

| Профиль | Описание | Когда использовать |
|---------|----------|-------------------|
| `fast_local` | Локальная модель, без RAG | Быстрая генерация, итерации |
| `accurate_cloud` | Облачная модель, с RAG | Сложные задачи, деплой |
| `secure_audit` | Строгая валидация | Аудит, ревью кода |
| `debug_verbose` | Максимальная детализация | Отладка, разработка |

Установка профиля:
```cpp
orchestrator->initialize(context);
// Профиль загружается автоматически из plugins/user_plugins/wp_coder/profiles/wp_coder/
```

## Примеры интеграции

### C++ (через C-API)
```cpp
#include "wp_coder_plugin.h"

// Создание агента
agents::IAgent* agent = plugin_create_agent("wp_theme_agent");
agent->initialize(context);

// Выполнение запроса
agents::AgentRequest request;
request.set_action("generate");
request.set_param("description", "Минималистичная тема для портфолио");

agents::AgentResult result = agent->execute(request);
if (result.is_success()) {
    std::string code = result.get<std::string>("theme_code");
    // Сохраняем файлы
}

agent->shutdown();
plugin_destroy_agent(agent);
```

### Python (через REST API хост-агента)
```python
import requests

# Генерация темы
response = requests.post(
    "http://localhost:8080/agent/execute",
    json={
        "agent": "wp_coder_orchestrator",
        "action": "generate_theme",
        "params": {"description": "Тема для интернет-магазина"}
    }
)

theme_files = response.json()["files"]
for filename, content in theme_files.items():
    with open(filename, "w") as f:
        f.write(content)
```

## Требования

- WordPress 5.8+
- PHP 7.4+
- LLM API ключ (OpenRouter/OpenAI) для генерации
- WP-CLI для деплоя
- rsync для синхронизации файлов

## Безопасность

- Все shell-команды проходят через белый список
- Файловые операции ограничены `wp-content/`
- Размер файлов ограничен 10 МБ
- Строгий режим: только разрешенные команды

## Лицензия

MIT — см. `LICENSE` файл.

## Поддержка

Для вопросов и баг-репортов: [GitHub Issues](https://github.com/gsemantic/llama-gui3/issues)