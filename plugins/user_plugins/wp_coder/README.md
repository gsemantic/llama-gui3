# AI Coder — Модульная архитектура

Плагин `wp_coder` — это полноценный AI-кодер с модульной архитектурой.
WordPress является одним из модулей. Другие модули (Python, DevOps и т.д.)
подключаются аналогично.

## Архитектура

```
wp_coder/
├── core/                          # Универсальное ядро
│   ├── module_api.h/cpp           # Интерфейс модулей + ModuleRegistry
│   ├── engine.h/cpp               # ReAct-движок (доменно-независимый)
│   ├── tools_registry.h/cpp       # Динамический реестр инструментов
│   ├── skills_manager.h/cpp       # Менеджер навыков
│   ├── security.h/cpp             # Безопасность (path validation, blocked commands)
│   ├── base_tools.h/cpp           # read/write, search_replace, repo_map, grep
│   ├── git_tools.h/cpp            # git status/diff/log/commit
│   ├── project.h/cpp              # Настройки проекта
│   └── prompts.h                  # Базовый системный промпт
├── modules/
│   ├── wordpress/                 # WP-специализация (12 инструментов, 6 навыков)
│   ├── python/                    # Python (6 инструментов, 4 навыка)
│   └── devops/                    # DevOps (11 инструментов, 3 навыка)
├── ui/                            # Модульный интерфейс
│   └── coder_window.h/cpp         # Окна: Проект, Модули, Инструменты
├── tests/                         # Unit-тесты (36 тестов)
└── src/plugin_main.cpp            # Точка входа плагина
```

## Модули

| Модуль | Инструменты | Навыки |
|--------|-------------|--------|
| **WordPress** | wp_cli, wp_db, wp_media, wp_option, wp_rest, wp_create_site, wp_check_deps, deploy, verify, php_lint, headless_render, validate | wp_theme, wp_hook, wp_database, wp_media, wp_plugin_boilerplate, wp_git |
| **Python** | python_run, pip_install, django_manage, pytest_run, venv_create, python_lint | python_django, python_flask, python_fastapi, python_project |
| **DevOps** | docker_build/run/ps/logs, systemd_status/restart, nginx_test/reload, cron_list/add, ssh_exec | devops_docker, devops_systemd, devops_nginx |

## Как это работает

1. При запуске плагин регистрирует модули через `ModuleRegistry`
2. Каждый модуль регистрирует свои инструменты в `ToolsRegistry`
3. Движок (Engine) собирает системный промпт: базовый + модульные промпты + навыки
4. Агент (ReAct-цикл) вызывает инструменты через `ToolsRegistry::run()`
5. Результаты отображаются в UI через `AgentEvent` ленту

## Сборка

```bash
cd build_modular
cmake ../plugins/user_plugins/wp_coder
cmake --build .
```

## Тесты

```bash
cd build_modular
./tests/wp_coder_tests
```

## Статистика

- **Инструментов**: 29 (12 WP + 6 Python + 11 DevOps)
- **Навыков**: 13 (6 WP + 4 Python + 3 DevOps)
- **Тестов**: 36 (все PASS)
- **Размер .so**: ~5 МБ
