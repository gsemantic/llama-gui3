# План работ — плагин news_rewriter (детальная проработка)

Дополняет `docs/ARCHITECTURE.md` (там — общая схема и мотивация). Здесь —
контракты модулей, файловая структура, пошаговый план задач с критериями
готовности, порядок реализации и журнал решений. Читать вместе с
`ARCHITECTURE.md`.

Статус: **этапы 0–2 реализованы; этап 3 (Rewriter) следующий.**

---

## 1. Файловая структура плагина

```
plugins/user_plugins/news_rewriter/
├── CMakeLists.txt              # цель news_rewriter (SHARED) + опция тестов
├── plugin.json                 # манифест (см. ARCHITECTURE.md)
├── README.md                   # краткое описание, сборка, использование
├── docs/
│   ├── ARCHITECTURE.md         # существующий документ (общая схема)
│   └── PLAN.md                 # этот документ
├── src/
│   ├── common.h                # Article, TaskStatus, лог-хелперы, sha256
│   ├── json.h / json.cpp       # мини JSON-парсер/сериализатор (без зависимостей)
│   ├── config.h / config.cpp   # структуры конфига + load/save через settings_*
│   ├── http.h / http.cpp       # обёртка над libcurl (dlopen, см. журнал решений)
│   ├── xml.h / xml.cpp         # мини XML-парсер (RSS 2.0 / Atom)
│   ├── fetcher.h / fetcher.cpp # загрузка по URL, разбор фидов/страниц
│   ├── extractor.h/.cpp        # HTML→текст + извлечение title/body│   ├── rewriter.h/.cpp         # промпт-шаблон + llm_complete (только worker)
│   ├── sink.h                  # интерфейс Sink + фабрика + реестр
│   ├── sink_local_file.cpp     # LocalFileSink (v1)
│   ├── storage.h / storage.cpp # структура каталогов, index.json, state.json, dedup
│   ├── scheduler.h/.cpp        # очередь задач, статусы, retry, таймер
│   ├── worker.h / worker.cpp   # рабочий поток + потокобезопасная очередь команд
│   └── ui.h / ui.cpp           # ImGui-окно (только main-поток)
└── tests/
    ├── test_main.cpp           # точка входа тестов плагина
    ├── test_framework.h        # мини-раннер тестов (no deps)
    ├── test_json.cpp
    ├── test_worker.cpp         # pipeline через FakeFetcher
    ├── test_http.cpp           # локальный HTTP-сервер (fixture)
    ├── test_server.h           # мини-HTTP-сервер на сокетах (для test_http)
    ├── test_xml.cpp
    ├── test_fetcher.cpp
    ├── test_extractor.cpp      # этап 2
    └── test_storage.cpp        # этап 4
```

`user_plugins/CMakeLists.txt` (добавляется сейчас):

```cmake
add_subdirectory(news_rewriter)
```

Корневой `plugins/CMakeLists.txt` уже подхватывает `user_plugins` при наличии
этого файла — правки основного проекта не требуются.

---

## 2. Контракты модулей

### 2.1 Общие типы (`common.h`)

```cpp
enum class TaskStatus {
    Pending, Fetching, Extracting, Rewriting, Exporting, Done, Error
};

struct Article {
    std::string id;              // sha256(url) — стабильный идентификатор
    std::string url;
    std::string source;          // метка источника (host из url или имя из config)
    std::string fetched_at;      // ISO-8601 UTC, "2026-08-08T12:00:00Z"
    std::string title_original;
    std::string body_original;
    std::string title_rewritten;
    std::string body_rewritten;
    std::string language;        // из config.rewrite.language
    TaskStatus status = TaskStatus::Pending;
    std::string error;           // текст ошибки (пусто = нет)
    uint32_t retry_count = 0;
    std::string content_hash;    // sha256(title_original + "\n" + body_original)
};
```

### 2.2 Конфиг (`config.h`)

```cpp
struct SourceExtract {           // для type="page": где брать title/body
    std::string title_marker;    // пусто = эвристика
    std::string body_marker;
};

struct SourceConfig {
    std::string url;
    std::string type;            // "rss" | "atom" | "page"
    SourceExtract extract;
    bool enabled = true;
};

struct RewriteConfig {
    std::string language = "ru";
    std::string tone = "нейтральный";
    std::string prompt_template; // "{title}\n{body}" → подстановки
};

struct SinkConfig {
    std::string type = "local_file";
    // тип-агностичные параметры (напр. url/ключ для HttpSink) — в params
    nlohmann_json_like params;   // сериализуемое JSON-значение
};

struct NetworkConfig {
    int timeout_seconds = 20;
    std::string user_agent = "news_rewriter/1.0";
    std::string proxy;           // пусто = системный прокси curl
    std::string extra_headers;   // "Header: value\n..." (для авторизации и пр.)
};

struct Config {
    std::vector<SourceConfig> sources;
    RewriteConfig rewrite;
    int schedule_minutes = 60;   // 0 = только ручной запуск
    SinkConfig sink;
    NetworkConfig network;
};
```

Хранение: `settings_get("news_rewriter.config")` — JSON-строка.
Загрузка: при `ll_plugin_init`; сохранение — после правок в UI и при shutdown.

### 2.3 HTTP (`http.h`)

```cpp
struct HttpResponse {
    bool ok = false;
    int status = 0;              // HTTP-код
    std::string body;
    std::string final_url;       // после редиректов
    std::string error;           // текст ошибки сети/таймаута
};

class HttpClient {
public:
    bool init();                 // dlopen libcurl, resolve символов
    HttpResponse get(const std::string& url, const NetworkConfig& cfg);
    // точка роста: post(), download_file() для будущих Sink-ов
};
```

Ограничения: таймаут соединения/чтения, лимит размера тела (напр. 5 МБ,
обрезается), до 10 редиректов, обязательный User-Agent, поддержка HTTPS,
`proxy` из конфига. Без куки-менеджмента в v1.

### 2.4 XML / RSS (`xml.h`, `fetcher.h`)

```cpp
struct FeedItem {
    std::string title;
    std::string link;
    std::string description;     // может содержать HTML
    std::string pub_date;        // необязательно
};

struct Feed {
    std::string channel_title;
    std::vector<FeedItem> items;
};

class XmlParser {
public:
    // DOM-подобное дерево, минимальное: узлы, атрибуты, текст, CDATA,
    // декодирование сущностей (&amp; &lt; &gt; &quot; &#NN;)
    bool parse(const std::string& xml);
};

Feed parse_feed(const XmlParser::Node* root); // RSS 2.0 и Atom
```

### 2.5 Extractor (`extractor.h`)

```cpp
struct ExtractedArticle {
    std::string title;
    std::string body;
};

ExtractedArticle extract_page(const std::string& html,
                              const SourceExtract& cfg);
ExtractedArticle extract_from_description(const std::string& desc); // HTML→текст
```

Логика v1:
- `extract_from_description`: снять теги (токенизатор), декодировать сущности,
  нормализовать пробелы/переносы, отсечь навигацию-строки.
- `extract_page`: если заданы маркеры — текст между ними; иначе эвристика:
  заголовок = `<h1>`/`<title>`, тело = самый длинный блок текста.
- Точка роста: алгоритм читаемости по плотности текста (этап 6).

### 2.6 Rewriter (`rewriter.h`)

```cpp
struct RewriteResult {
    bool ok = false;
    std::string title;
    std::string body;
    std::string error;
};

RewriteResult rewrite(const Article& src, const RewriteConfig& cfg,
                      RewriteLlm& llm);
```

`RewriteLlm` — тонкая обёртка над `g_api->llm_complete` (живёт только в worker).
Промпт собирается по шаблону `config.rewrite.prompt_template` с подстановками
`{title}`, `{body}`, `{language}`, `{tone}`. Ответ разбивается: первая непустая
строка — заголовок (опционально, если в шаблоне запрошено), остальное — тело.
Обработка: `llm_complete != 1` / пустой ответ / таймаут → `ok=false`, текст
ошибки в `Article.error`.

### 2.7 Sink (`sink.h`) — ключ масштабируемости

```cpp
class Sink {
public:
    virtual ~Sink() = default;
    virtual bool write(const Article& article) = 0;
    virtual const char* name() const = 0;
};

using SinkFactory = std::unique_ptr<Sink>(*)(const SinkConfig&, Storage&, const LogFn&);

class SinkRegistry {
public:
    static SinkRegistry& instance();
    void register_factory(const char* type, SinkFactory f);
    std::unique_ptr<Sink> create(const SinkConfig& cfg, Storage& st, const LogFn& log);
};
```

- v1: `LocalFileSink` (type `"local_file"`), регистрируется в `ll_plugin_init`.
- Этап 6: `HttpSink` (type `"http"`) — регистрируется своим модулем, добавляется
  **без правок** fetcher/extractor/rewriter/worker. Активный sink выбирается в
  конфиге.
- `Storage&` передаётся, чтобы Sink не знал про каталоги, а только писал файлы
  через интерфейс Storage (единая точка для index/state).

### 2.8 Storage (`storage.h`)

```cpp
class Storage {
public:
    // root = path_data_dir()/news_rewriter/
    bool init(const char* data_dir);      // создаёт структуру каталогов
    std::string slug_for(const std::string& url);
    std::string article_json_path(const Article& a);
    std::string article_md_path(const Article& a);
    bool save_article_json(const Article& a);   // метаданные + текст
    bool save_article_md(const Article& a);     // человекочитаемый рерайт
    bool load_index(JsonDoc& out);              // url → локальный файл
    bool save_index(const JsonDoc& index);
    bool load_state(JsonDoc& out);              // прогресс/статусы/счётчики
    bool save_state(const JsonDoc& state);
    bool is_duplicate(const Article& a);        // по id или content_hash
    void mark_written(const Article& a);        // занесение в index
};
```

Каталоги:
```
news_rewriter/
├── articles/<YYYY-MM-DD>/<slug>.json
├── articles/<YYYY-MM-DD>/<slug>.md
├── index.json      # { "<id>": "<относительный путь файла>" }
└── state.json      # { "last_run": "...", "tasks": { "<id>": {...} } }
```

Дедупликация: id = sha256(url); дополнительно content_hash сравнивается с
последними N записей (против «тот же текст по новому URL»). Повторный обход
того же URL не создаёт дубликата.

### 2.9 Scheduler (`scheduler.h`)

```cpp
struct TaskHandle { std::string article_id; TaskStatus status; };

class Scheduler {
public:
    void configure(const Config& cfg);
    void run_now();                     // форсирует проход всех включённых источников
    // основной цикл в worker: fetch→extract→rewrite→sink для каждой задачи
    std::vector<Article> drain_done();  // результаты для UI (snapshot)
    bool has_pending() const;
    void stop();
};
```

Статусы задач и ретраи: при сбое шага `TaskStatus::Error` + `retry_count++`;
после `max_retries` (по умолчанию 3) задача помечается `Error` окончательно и
не блокирует очередь. Backoff: 5 с → 30 с → 5 мин (кумулятивно от числа
ретраев).

### 2.10 Worker + очередь команд (`worker.h`) — потоковая модель

```
main-поток (ll_plugin_render)                worker-поток (std::thread)
─────────────────────────────                ────────────────────────────
ImGui-ввод / кнопки                    ┌──►  цикл: ждать команду/таймер
  │                                     │         │
  └─► command_queue.push(Cmd) ─────────►│         ▼
        (mutex+condvar)                 │   run pipeline (scheduler)
                                        │         fetch→extract→rewrite→sink
UI читает SNAPSHOT (зам. lock) ◄────────┘   состояние задач → snapshot
```

```cpp
enum class CmdType { RunNow, Stop, ReloadConfig, SetSourceEnabled };
struct Command { CmdType type; std::string arg; };

class Worker {
public:
    bool start();                 // создаёт поток
    void post(Command cmd);       // thread-safe
    StatusSnapshot snapshot() const; // для UI (mutex)
    void stop_and_join();         // в ll_plugin_shutdown
private:
    void loop();                  // condvar wait / таймер расписания
};
```

Правила (из ARCHITECTURE.md, закрепляются контрактом):
- ImGui вызывается **только** в main-потоке (`ll_plugin_render`).
- `g_api->llm_complete` — **только** в worker.
- UI никогда не зовёт блокирующие функции; только `post()` + чтение snapshot.
- `ll_plugin_shutdown`: `stop` → `join` (с таймаутом 5 с) → сохранить config/state.

---

## 3. Пошаговый план задач

Обозначения задач: `ID — описание — файлы — критерий готовности`.
Зависимости: этап N зависит от (N-1). Внутри этапа задачи можно делать в любом
порядке, если не указано иное.

### Этап 0 — Каркас (инфраструктура)

| # | Задача | Файлы | Критерий готовности |
|---|---|---|---|
| 0.1 | Создать `user_plugins/CMakeLists.txt` + каркас CMake-цели | `user_plugins/CMakeLists.txt`, `news_rewriter/CMakeLists.txt` | `cmake --build build` собирает `build/plugins/libnews_rewriter.so` и копирует `plugin.json` рядом |
| 0.2 | Точка входа плагина: экспорт `ll_plugin_*`, регистрация команды `news_rewriter_run`, пункта меню, окна | `src/plugin_main.cpp` | Плагин загружается в `tests/core/test_plugin_loader`, окно открывается из меню, команда срабатывает |
| 0.3 | Мини JSON-модуль + юнит-тесты | `src/json.h/.cpp`, `tests/test_json.cpp` | Парс/сериализация вложенных объектов, массивов, чисел, строк, экранирование — тесты зелёные |
| 0.4 | Конфиг: структуры + load/save через `settings_get/set` с дефолтами | `src/config.h/.cpp` | Настройки переживают перезапуск; дефолтный конфиг создаётся при первом запуске |
| 0.5 | Worker + очередь команд + пустое окно UI (без pipeline) | `src/worker.h/.cpp`, `src/ui.h/.cpp` | Кнопка «Обойти сейчас» ставит команду, статус виден в UI, UI не зависает |
| 0.6 | Манифест `plugin.json`, README | `plugin.json`, `README.md` | Манифест валиден по `plugins/plugin.schema.json`; README описывает сборку и запуск |

**Критерий этапа:** `.so` грузится, окно/меню/команда работают, конфиг
персистентен, никаких зависаний UI. Коммит.

### Этап 1 — Fetcher ✅

| # | Задача | Файлы | Критерий готовности | Статус |
|---|---|---|---|---|
| 1.1 | HTTP-обёртка над libcurl (dlopen, таймауты, лимит, редиректы, UA, прокси) | `src/http.h/.cpp`, `tests/test_http.cpp` | GET отдаёт тело/код/ошибку; таймаут и обрезка работают (локальный сервер в тесте) | ✅ |
| 1.2 | Мини XML-парсер + извлечение RSS/Atom | `src/xml.h/.cpp` | Парсит тестовые фиды RSS 2.0 и Atom; CDATA и сущности корректны | ✅ |
| 1.3 | Fetcher: загрузка по URL, выбор ветки rss/atom/page | `src/fetcher.h/.cpp` | По URL возвращаются `FeedItem`-ы или сырой HTML + статус/ошибка | ✅ |
| 1.4 | Включить fetch-ветку в worker pipeline (без рерайта), лог результатов | `src/worker.cpp` | Команда «Обойти сейчас» грузит все включённые источники; статьи статуса `Pending→Fetching→Done/Error` в журнале | ✅ |

**Критерий этапа:** реальный RSS-фид грузится и выводится в лог/список без
обработки текста. Коммит. — **выполнен.**

### Этап 2 — Extractor ✅

| # | Задача | Файлы | Критерий готовности | Статус |
|---|---|---|---|---|
| 2.1 | HTML→текст: теги, сущности, нормализация | `src/extractor.cpp` | Тест на образцах HTML даёт чистый текст без рекламы/скриптов/меню | ✅ |
| 2.2 | Извлечение title/body по маркерам + эвристика | `src/extractor.h/.cpp`, `tests/test_extractor.cpp` | Из образцов страниц/описаний достаются корректные заголовок и текст | ✅ |
| 2.3 | Wire extractor в pipeline, стадии `Extracting` | `src/worker.cpp` | В snapshot видны `title_original`/`body_original` | ✅ |

**Критерий этапа:** по реальным страницам извлекается связный текст. Коммит. — **выполнен.**

### Этап 3 — Rewriter

| # | Задача | Файлы | Критерий готовности |
|---|---|---|---|
| 3.1 | Обёртка LLM + сборка промпта по шаблону, разбор ответа | `src/rewriter.h/.cpp` | Промпт собирается с подстановками; ошибки/пустой ответ обрабатываются |
| 3.2 | Pipeline: `Rewriting` стадия, статусы статей в UI | `src/worker.cpp`, `src/scheduler.cpp`, `src/ui.cpp` | Статьи переписаны через `llm_complete`, статусы обновляются в реальном времени, UI не «висит» на длинном рерайте |
| 3.3 | Обработка недоступности LLM (сервер/облако) | `src/rewriter.cpp` | При `llm_complete == 0` статья → `Error` с понятным текстом, очередь не блокируется |

**Критерий этапа:** полный цикл fetch→extract→rewrite работает; ошибки не
вешают UI и не останавливают очередь. Коммит.

### Этап 4 — Sink / Storage

| # | Задача | Файлы | Критерий готовности |
|---|---|---|---|
| 4.1 | Интерфейс `Sink`, реестр+фабрика | `src/sink.h` | Регистрация и создание sink по имени из конфига |
| 4.2 | Storage: каталоги, json/md, index, state | `src/storage.h/.cpp`, `tests/test_storage.cpp` | Файлы в структуре `articles/<дата>/<slug>.{json,md}`; index/state согласованы |
| 4.3 | LocalFileSink | `src/sink_local_file.cpp` | Запись на диск, обработка ошибок записи |
| 4.4 | Дедупликация (id + content_hash) | `src/storage.cpp` | Повторный обход того же URL не дублирует; новый URL с тем же текстом помечается дублем |
| 4.5 | Выбор активного sink из конфига, `Exporting` стадия | `src/worker.cpp`, `src/config.cpp`, `src/ui.cpp` | Смена sink в конфиге меняет поведение без пересборки ядра плагина |

**Критерий этапа:** готовые `.json`/`.md` на диске; повторные запуски без
дублей. Коммит.

### Этап 5 — Scheduler

| # | Задача | Файлы | Критерий готовности |
|---|---|---|---|
| 5.1 | Таймер расписания (condvar `wait_until`) + ручной запуск | `src/scheduler.h/.cpp`, `src/worker.cpp` | Автономный обход по `schedule_minutes` без участия пользователя; смена интервала применяется |
| 5.2 | Retry + backoff, max_retries | `src/scheduler.cpp` | Сбои уходят в retry с задержками; исчерпание → `Error` без блокировки очереди |
| 5.3 | Чистое завершение: stop → join → save | `src/worker.cpp`, `src/plugin_main.cpp` | `ll_plugin_shutdown` завершает поток за ≤5 с, конфиг/state сохранены |
| 5.4 | Статус-бар/прогресс в UI (активен ли worker, число задач) | `src/ui.cpp` | UI показывает текущее состояние цикла |

**Критерий этапа:** плагин работает автономно по расписанию, останавливается
чисто. Коммит.

### Этап 6 — Расширения (масштабируемость)

| # | Задача | Файлы | Критерий готовности |
|---|---|---|---|
| 6.1 | `HttpSink` (отправка на сервер: URL, API-ключ, ретраи) | `src/sink_http.cpp` (новый) | Выбирается из конфига; работает без правок ядра (проверка контракта Sink) |
| 6.2 | Прокси/авторизация в HttpClient (`proxy`, `extra_headers`) | `src/http.cpp` | Уже заложено в `NetworkConfig`; интеграционный тест с локальным сервером |
| 6.3 | Универсальный extractor (плотность текста) | `src/extractor.cpp` | Работает на страницах без маркеров; сравнимо с эвристикой на образцах |
| 6.4 | Обновить ARCHITECTURE/PLAN по факту реализации | docs | Документация соответствует коду |

**Критерий этапа:** новые возможности подключаются через конфиг/реестр без
правок ядра плагина.

---

## 4. Порядок реализации и критический путь

```
Зависимости:  0 → 1 → 2 → 3 → 4 → 5
                \           └→ 6 (параллельно/после 4)
Критический путь: 0 → 1 → 2 → 3 → 4 → 5
```

- **Быстрый первый результат:** этап 0 даёт работающий каркас уже в первую
  итерацию (можно проверять загрузку в GUI).
- **Ветвление после 0.3:** json и config независимы — делать параллельно.
- **Http/XML на этапе 1** — самые рискованные по внешним зависимостям части;
  их изоляция (dlopen, мини-парсеры) — приоритет.
- **Sink-интерфейс на этапе 4** — закладываем до появления HttpSink, чтобы
  этап 6 был «по контракту», а не рефакторингом.

Рекомендация по реализации: этап 0 полностью, затем этапы 1–5 последовательно
(каждый — рабочий `.so` + коммит). Этап 6 — по мере надобности.

---

## 5. План тестирования

| Уровень | Инструмент | Что покрываем |
|---|---|---|
| Юнит-тесты плагина | Своя цель `news_rewriter_tests` (опция `BUILD_NEWS_REWRITER_TESTS`, использует копию `tests/test_framework.h` из приложения или свой мини-раннер) | json, xml, extractor, storage, http (локальный сервер на std::thread+socket) |
| Интеграция с хостом | `tests/core/test_plugin_loader` приложения | Загрузка/выгрузка `libnews_rewriter.so`, экспорт обязательных функций |
| Ручной | GUI: запуск, меню, окно, обход реальных фидов | Поведение, отсутствие зависаний, файлы на диске |

Тесты плагина не входят в корневой `ctest`, пока не интегрированы — это
сознательно: плагин независим. Подключение к `ctest` — отдельная задача
(вынести в корень при необходимости).

---

## 6. Журнал решений и риски

| Решение | Обоснование | Риск / следствие | Статус |
|---|---|---|---|
| libcurl через `dlopen` (runtime) | Полная независимость от системы сборки; в системе есть `libcurl.so.4` (7.88.1) | Версии curl в системе; при отсутствии — плагин пишет ошибку и не делает fetch | Зафиксировано; запасной путь: статический `find_package(CURL)` |
| Свой мини JSON/XML | Ноль внешних зависимостей, полная изоляция плагина | Объём кода; ограниченная функциональность | Принято для v1 |
| Worker-поток + очередь команд | Обходит блокирующий `llm_complete` и сеть без зависания UI | Синхронизация; утечки при неверном shutdown | Принято; контракт в 2.10 |
| Интерфейс `Sink` + реестр | Масштабируемость: HttpSink/EmailSink/RagIndexSink без правок ядра | Избыточность для одного sink | Принято — ключевое решение для будущих функций |
| Настройки через `settings_*` (JSON-строка) | Единый механизм persist приложения | Ключ один на весь конфиг; при больших конфигах — перезапись целиком | Принято для v1 |
| Дедупликация по URL+content_hash | Простота и надёжность | Не ловит смысловые дубли (разные формулировки одной новости) | Зафиксировано как будущая задача (этап 6.5+) |
| `llm_complete` только в worker | Избегаем зависания UI (документировано в PLUGIN_SYSTEM.md) | Очередь рерайтов блокирует последующие задачи до ответа LLM | Принято; ретраи не блокируют |

**Открытые вопросы (требуют решения перед этапами 3–4):**
1. Хранить ли переписанный текст в RAG-индексе (через `rag_process_document`)?
   → требует решения на этапе 4/6.
2. Формат отправки на сервер для HttpSink (JSON? поле подписи/подпись?) — на
   этапе 6.
3. Локализация UI (ru/en)? — на этапе 0, по умолчанию ru.

---

## 7. Критерии готовности v1

- Плагин собирается командой `cmake --build build` без правок основного проекта.
- `build/plugins/libnews_rewriter.so` + `plugin.json` загружаются приложением.
- Реальные RSS/Atom/страницы: обход → извлечение → рерайт → `.json`/`.md` на
  диске, без дублей при повторных запусках.
- Автономное расписание + ручной запуск; чистое завершение.
- Расширения (отправка на сервер) подключаются новым модулем без правок ядра.
