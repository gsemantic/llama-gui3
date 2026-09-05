# Changelog

## [0.2.0] - 2026-09-02

### Added
- Git-интеграция: `git_status`, `git_diff`, `git_log`, `git_commit`
- Окно «Git» в UI с кнопками для быстрых операций
- Навык `wp_git.md` для работы с Git в проектах WordPress
- Инструмент `wp_db` для SQL-запросов через wp db
- Инструмент `wp_media` для работы с медиафайлами
- Инструмент `wp_option` для чтения опций WordPress
- Навык `wp_database.md` для работы с БД
- Навык `wp_media.md` для работы с медиафайлами
- Окно «Файлы» с деревом файлов и фильтрацией
- Горячие клавиши: Ctrl+Shift+G (Git), Ctrl+Shift+F (Файлы)
- Документация: USAGE.md, IMPROVEMENTS.md, README.md, CHANGELOG.md

### Changed
- Обновлен системный промпт с описанием новых инструментов
- Добавлены ссылки на навыки для Git в системном промпте
- Улучшены инструкции по работе с инструментами

### Improved
- Улучшен UI с дополнительными окнами
- Расширены возможности навигации по проекту
- Добавлена поддержка SQL-запросов
- Добавлена работа с медиафайлами и опциями WordPress

## [0.1.0] - 2026-08-26

### Added
- MVP-каркас плагина
- Окна «Проект» и «Агент»
- Worker-поток с ReAct-циклом
- Инструменты: `read_file`, `write_file`, `grep_hooks`, `php_lint`
- Инструменты: `wp_cli`, `headless_render`
- Инструменты: `rag_index`, `rag_query`, `repo_map`
- Инструменты: `wp_rest`, `validate`, `verify`, `deploy`
- Навыки: `wp_hook`, `wp_theme`, `wp_plugin_boilerplate`
- План-режим для безопасного редактирования
- Ролевые режимы: Code, Research, Review
- Деплой через rsync или внешний deploy.sh