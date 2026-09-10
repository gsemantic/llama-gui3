# Changelog

## [0.3.0] - 2026-09-10

### Added
- Модульная архитектура: `core/` + `modules/{wordpress,python,devops}` + `ui/`
- Компоненты ReAct-цикла (D1): `SessionStore`, `PermissionGate`, `ToolRunner`,
  `Planner`, `AgentLoop` в `core/agent_components.{h,cpp}`
- Парсер протокола `tool_protocol.{h,cpp}` (D2): wp_action + JSON tool_calls
- Multi-turn сессия: `HostCallbacks.llm_chat` (история диалога → provider-side caching)
- Худой системный промпт (~3 Кб): навыки только описаниями, тела через `skill_detail`
- `trim_session` при превышении 60 Кб — сжатие старых RESULT/assistant-сообщений
- Таймауты в shell-инструментах (`shell.h`: `run_capture`, `shell_quote`)
- `grep_search` с regex + кап вывода 12 Кб на всех инструментах
- Кнопка «Стоп» (`Engine::request_abort`), честные метрики tok/s из usage API
- Разрешения (`check_external_permission`): read/write/search_replace для путей вне проекта
- Agent mode `ai_coder` для основного чата приложения
- Окна «Проект» (`Ctrl+Shift+W`), «Модули» (`Ctrl+Shift+M`), «Инструменты» (`Ctrl+Shift+T`)
- Флаг `--agent=NAME` в `main.cpp` для тестирования плагинного агента без GUI

### Changed
- `Engine::run_task` делегирует `Planner` (B1) + `AgentLoop` (C) вместо монолитного цикла
- Навыки грузятся inline из модулей + `.md` из `skills/` (с дедупом по имени — приоритет за inline)

### Removed
- Устаревшие документы эпохи монолита (PROGRESS, CONCLUSION, FINAL_REPORT, SUMMARY, IMPROVEMENTS)
- Дублирующие `.md` навыки в `modules/wordpress/skills/` (идентичны `skills/`)

### Stats
- Инструментов: 43 (10 базовых + 4 git + 12 WP + 6 Python + 11 DevOps)
- Навыков: 14 (13 inline + 1 внешний `skills/wp_setup.md`)
- Тестов: 48 (все PASS)
- Размер .so: ~670 Кб

## [0.2.0] - 2026-09-02

### Added
- Git-интеграция: `git_status`, `git_diff`, `git_log`, `git_commit`
- Инструмент `wp_db` (SQL через `wp db query`), `wp_media`, `wp_option`
- Навыки `wp_database`, `wp_media`, `wp_git`
- Окно «Файлы» с деревом файлов и фильтрацией
- Горячие клавиши: Ctrl+Shift+G (Git), Ctrl+Shift+F (Файлы)

## [0.1.0] - 2026-08-26

### Added
- MVP-каркас плагина: окна «Проект»/«Агент», worker-поток с ReAct-циклом
- Базовые инструменты: `read_file`, `write_file`, `grep_hooks`, `php_lint`, `repo_map`
- WordPress-инструменты: `wp_cli`, `headless_render`, `wp_rest`, `validate`, `verify`, `deploy`
- RAG: `rag_index`, `rag_query`
- Навыки: `wp_hook`, `wp_theme`, `wp_plugin_boilerplate`
- План-режим, ролевые режимы (Code/Research/Review)
