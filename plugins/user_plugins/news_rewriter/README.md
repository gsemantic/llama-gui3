# news_rewriter — плагин-агент для llama-gui

Плагин автономно обходит указанные адреса (RSS/Atom/страницы), извлекает
новости, переписывает их через LLM (локальный сервер llama-gui или облако)
и сохраняет результаты локально.

Разрабатывается независимо от основного проекта — подключает только SDK
`include/plugins/plugin_api.h` и Dear ImGui. Подробности: `docs/ARCHITECTURE.md`
(схема), `docs/PLAN.md` (контракты, этапы, задачи).

## Статус реализации

| Этап | Название | Статус |
|---|---|---|
| 0 | Каркас (окно, команды, конфиг, worker) | **реализован** |
| 1 | Fetcher (HTTP + RSS/Atom) | **реализован** |
| 2 | Extractor (HTML → текст) | **реализован** |
| 3 | Rewriter (LLM) | запланирован |
| 4 | Sink / Storage (диск, дедупликация) | запланирован |
| 5 | Scheduler (расписание, retry) | запланирован |
| 6 | Расширения (отправка на сервер и пр.) | запланирован |

## Сборка

```bash
# Из корня проекта llama-gui:
cmake -S . -B build        # уже есть build/
cmake --build build
```

Результат: `build/plugins/libnews_rewriter.so` + `build/plugins/news_rewriter.json`.

## Запуск

Положить `.so` и манифест в директорию плагинов приложения (по умолчанию
`build/plugins/`) и запустить GUI — плагин загрузится автоматически
(см. лог `[PluginManager] Loaded plugin: news_rewriter`).

- Меню **Agents → Open News Rewriter** — окно плагина.
- Кнопка **«Обойти сейчас»** — запуск обхода. На этапе 2 работает полный fetch+extract:
  - `rss`/`atom` — статьи извлекаются: заголовок очищается от HTML, описание
    приводится к чистому тексту, статьи появляются в списке.
  - `page` — по маркерам или эвристике (заголовок `<h1>`/`<title>`, тело —
    самый длинный блок) извлекается текст страницы.
  - Рерайт текста — с этапа 3.

## Тесты

```bash
cmake -S . -B build -DBUILD_NEWS_REWRITER_TESTS=ON
cmake --build build --target news_rewriter_tests
./build/plugins/news_rewriter_tests   # или build/... в зависимости от layout
```

## Структура

```
src/plugin_main.cpp   — точка входа ll_plugin_*, связывание модулей
src/common.*          — Article, статусы, SHA-256, ISO-8601
src/json.*            — мини JSON (без внешних зависимостей)
src/config.*          — структуры и сериализация конфига
src/http.*            — HTTP-клиент на libcurl (dlopen, этап 1)
src/xml.*             — мини XML-парсер RSS 2.0 / Atom (этап 1)
src/fetcher.*         — загрузка по URL + разбор фидов (этап 1)
src/extractor.*       — HTML→текст, title/body по маркерам/эвристике (этап 2)
src/worker.*          — фоновый поток + очередь команд + pipeline
src/ui.*              — ImGui-окно (только main-поток)
tests/                — юнит-тесты (json, http, xml, fetcher, extractor, worker)
```
