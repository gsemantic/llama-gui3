# WP Coder — AI-кодер для WordPress

## Описание

WP Coder — это плагин для Llama GUI, который позволяет использовать AI для разработки, редактирования и администрирования WordPress сайтов.

## Возможности

- **Генерация кода** — создание плагинов, тем, хуков
- **Редактирование** — безопасное редактирование файлов с план-режимом
- **Администрирование** — управление WordPress через wp-cli
- **Интеграция с Git** — контроль версий для проектов
- **RAG-индексация** — поиск по проиндексированному коду
- **Деплой** — автоматическая публикация на хостере

## Быстрый старт

1. Соберите плагин:
```bash
cmake --build build --target wp_coder
```

2. Скопируйте конфигурацию:
```bash
cp plugins/user_plugins/wp_coder/plugin.json build/plugins/wp_coder.json
```

3. Запустите GUI:
```bash
./run_gui.sh
```

4. Настройте проект в меню **WordPress → Проект**

## Структура плагина

```
wp_coder/
├── src/
│   ├── wp_coder.h          # Основные структуры данных
│   ├── plugin_main.cpp     # UI и точка входа
│   ├── agent.cpp           # AI-агент с ReAct-циклом
│   ├── tools.cpp           # Реализация инструментов
│   └── project.cpp         # Управление проектом
├── skills/
│   ├── wp_hook.md          # Навык: хуки WordPress
│   ├── wp_theme.md         # Навык: темы WordPress
│   ├── wp_plugin_boilerplate.md # Навык: плагины
│   ├── wp_git.md           # Навык: Git
│   ├── wp_database.md      # Навык: БД
│   └── wp_media.md         # Навык: медиафайлы
├── CMakeLists.txt
├── plugin.json
├── PROGRESS.md             # Прогресс разработки
├── USAGE.md                # Руководство пользователя
├── IMPROVEMENTS.md         # Список улучшений
├── README.md               # Этот файл
├── CHANGELOG.md            # История изменений
├── SUMMARY.md              # Итоговая сводка
├── FINAL_REPORT.md         # Финальный отчет
└── CONCLUSION.md           # Заключение
```

## Инструменты

### Базовые
- `read_file` — чтение файла
- `write_file` — запись файла
- `grep_hooks` — поиск хуков
- `php_lint` — проверка синтаксиса

### WordPress
- `wp_cli` — команды wp-cli
- `wp_rest` — REST API запросы
- `wp_db` — SQL-запросы
- `wp_media` — медиафайлы
- `wp_option` — опции WordPress

### Разработка
- `repo_map` — обзор проекта
- `rag_index` — индексация в RAG
- `rag_query` — поиск по RAG
- `validate` — проверка всех файлов
- `verify` — полная проверка

### Git
- `git_status` — статус репозитория
- `git_diff` — разница с HEAD
- `git_log` — история коммитов
- `git_commit` — создание коммита

### Системные
- `list_skills` — список навыков
- `deploy` — деплой на хостер

## Навыки

Плагин включает готовые навыки:
- **wp_hook** — правила использования хуков
- **wp_theme** — работа с темами
- **wp_plugin_boilerplate** — каркас плагина
- **wp_git** — работа с Git
- **wp_database** — работа с БД
- **wp_media** — работа с медиафайлами

### Создание своих навыков

Создайте `.md` файл в `<data_dir>/wp_coder/skills/`:

```markdown
# my_skill
Описание: краткое описание навыка
Текст инструкции...
```

## Режимы работы

1. **Code** — полный доступ ко всем инструментам
2. **Research** — только чтение
3. **Review** — после правок автоматически запускает verify

## Горячие клавиши

- **Ctrl+Shift+W** — окно «Проект»
- **Ctrl+Shift+E** — окно «Агент»
- **Ctrl+Shift+G** — окно «Git»
- **Ctrl+Shift+F** — окно «Файлы»

## Требования

- Llama GUI 0.1.60+
- PHP CLI (для проверки синтаксиса)
- wp-cli (для команд WordPress)
- Git (для интеграции с версиями)
- Chromium (для headless-рендера, опционально)

## Лицензия

MIT License

## Автор

llama-gui project