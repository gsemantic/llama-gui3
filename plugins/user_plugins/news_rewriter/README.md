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
| 3 | Rewriter (LLM) | **реализован** |
| 4 | Sink / Storage (диск, дедупликация) | **реализован** |
| 5 | Scheduler (расписание, retry) | **реализован** |
| 6 | Расширения (отправка на сервер и пр.) | 6.1 HttpSink — **реализован**; 6.2–6.3 — запланированы |

## Сборка

Плагин собирается автономно, без полной сборки GUI — его `CMakeLists.txt` сам
находит `include/` и `external/imgui` репозитория. Для быстрой итерации есть
скрипт `build.sh` (в каталоге плагина):

```bash
./build.sh                # конфигурация (тесты включены) + сборка .so и тестов
./build.sh --tests        # то же + прогон юнит-тестов
./build.sh --deploy DIR   # сборка + копирование .so и news_rewriter.json в DIR
./build.sh --clean        # удалить каталог сборки (build/)
./build.sh --help         # справка
```

Результат (автономно): `build/plugins/libnews_rewriter.so` +
`build/plugins/news_rewriter.json`. Вручную это делается так:

```bash
cmake -S plugins/user_plugins/news_rewriter -B build-nr -DBUILD_NEWS_REWRITER_TESTS=ON
cmake --build build-nr --target news_rewriter news_rewriter_tests
```

Плагин также собирается в составе основного проекта (`cmake --build build`).

## Запуск

Положить `.so` и манифест в директорию плагинов приложения (по умолчанию
`build/plugins/`) и запустить GUI — плагин загрузится автоматически
(см. лог `[PluginManager] Loaded plugin: news_rewriter`).

- Меню **Agents → Open News Rewriter** — окно плагина.
- Кнопка **«Обойти сейчас»** — запуск полного цикла fetch→extract→rewrite:
  - `rss`/`atom` — статьи извлекаются: заголовок очищается от HTML, описание
    приводится к чистому тексту.
  - `page` — по маркерам или эвристике (заголовок `<h1>`/`<title>`, тело —
    самый длинный блок) извлекается текст страницы.
  - Каждая статья рерайтится через `llm_complete` (если сервер/облако
    подключены); при недоступности LLM статья → `Error` с понятным текстом.
  - Переписанная статья пишется через активный sink (`local_file` по
    умолчанию): `.json` (метаданные + текст) и `.md` (человекочитаемый рерайт)
    в `path_data_dir()/news_rewriter/articles/<YYYY-MM-DD>/<slug>.{json,md}`;
    `index.json` и `state.json` обновляются. Дубли (повторный URL или тот же
    текст) отсекаются по id/content_hash, без перезаписи.
- **Расписание** (этап 5): при `schedule_minutes > 0` обход запускается
  автоматически по таймеру; статус и время до следующего запуска видны в окне.
  Сетевые сбои источника уходят в повторные попытки с backoff (5 с → 30 с →
  5 мин, `max_retries=3`); кнопка «Остановить обход» прерывает текущий обход и
  backoff мгновенно.
- **Sink `http`** (этап 6.1): вместо локальной записи статьи можно отправлять
  на сервер JSON-POST'ом (`article_to_json`, успех — HTTP 2xx). Параметры в
  конфиге (`sink.params`): `url` (обязательный), `api_key` (→ заголовок
  `Authorization: Bearer <key>`), `timeout_seconds` (20), `max_retries` (0),
  `retry_delay_ms` (1000). Дедупликация остаётся на `index.json`.

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
src/rewriter.*        — промпт + рерайт через LLM, разбор ответа (этап 3)
src/sink.h            — интерфейс Sink + реестр/фабрика (этап 4)
src/sink_local_file.cpp — LocalFileSink: запись .json/.md на диск (этап 4)
src/sink_http.cpp     — HttpSink: отправка статей JSON-POST на сервер (этап 6.1)
src/storage.*         — каталоги, index.json, state.json, дедупликация (этап 4)
src/scheduler.*       — расписание + политика ретраев (этап 5)
src/worker.*          — фоновый поток + очередь команд + pipeline + таймер
src/ui.*              — ImGui-окно (только main-поток)
tests/                — юнит-тесты (json, http, xml, fetcher, extractor, rewriter, scheduler, storage, worker)
```
