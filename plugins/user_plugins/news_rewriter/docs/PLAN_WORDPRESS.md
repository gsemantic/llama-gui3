# План: публикация рерайтов на удалённом WordPress (WordPressSink)

Дополнение к `docs/PLAN.md`. Плагин уже имеет расширяемый интерфейс `Sink` и
реализованный `HttpSink` (POST JSON на произвольный endpoint). Задача — добавить
новый sink типа `"wordpress"`, который публикует готовую статью в WordPress через
его REST API (`/wp-json/wp/v2/posts`), **без правок ядра** плагина (fetcher /
extractor / rewriter / worker не меняются).

Статус: проект (не реализовано).

---

## 0. ПРОГРЕСС (для быстрого возобновления в новой сессии)

> Решение по авторизации: оставляем **REST + Application Password (HTTP Basic)**,
> как в исходном плане. SSH не используем (обоснование — см. обсуждение сессии:
> REST пишет в БД штатно, доступен на любом хостинге, SSH даёт избыточный
> shell-доступ и требует shell на хосте).

**Как собрать и проверить (одна команда из `plugins/user_plugins/news_rewriter/`):**
```bash
./build.sh --tests        # сборка .so + тестов и прогон юнит-тестов
```
Тесты WordPress: `tests/test_sink_wordpress.cpp` (цель `news_rewriter_tests`).

**Статус задач:**
| # | Задача | Файл(ы) | Статус |
|---|---|---|---|
| 7.1 | `WordPressSink`: POST `/wp/v2/posts` + Basic-авторизация + маппинг полей | `src/sink_wordpress.cpp`, `src/sink.h`, `src/plugin_main.cpp` | ✅ сделано |
| 7.2 | `body_rewritten` → HTML (абзацы + экранирование + базовый Markdown) | `src/sink_wordpress.cpp` | ✅ сделано |
| 7.3 | Загрузка `featured_media` (опц.) | `src/sink_wordpress.cpp` | ✅ сделано (код, ручной тест WP) |
| 7.4 | Тесты `test_sink_wordpress.cpp` | `tests/test_sink_wordpress.cpp` | ✅ сделано (+ `credentials_from_env`) |
| — | Регистрация фабрики в `SinkRegistry` (без правок ядра) | `src/plugin_main.cpp` | ✅ сделано |
| — | Подключение исходников в `CMakeLists.txt` | `CMakeLists.txt` | ✅ сделано |
| — | Хранение секретов в `.env` (НЕ в settings.ini) — решение A | `src/dotenv.{h,cpp}`, `src/sink_wordpress.cpp`, `src/ui.cpp` | ✅ сделано |
| — | UI-поля WordPress (site_url, status, categories, tags, author, excerpt, slug, featured_image, max_retries, пароль-поле) | `src/ui.cpp`, `src/ui.h` | ✅ сделано |
| — | Кнопка «Проверить подключение» (GET /wp/v2/users/me, статус авторизации) | `src/sink_wordpress.cpp` (`wordpress_check_connection`), `src/ui.cpp`, `src/http.cpp` (перегрузка `get` с заголовками) | ✅ сделано |
| 7.5 | Расширение таксономии: `post_type` (posts/pages/кастомные CPT) — endpoint строится по slug, страницы не шлют категории/теги, листинг доступных типов в проверке связи | `src/sink_wordpress.cpp`, `src/ui.cpp`, `tests/test_sink_wordpress.cpp` | ✅ сделано |
| — | Бамп версии до 0.1.1 (видно в логе загрузки) | `src/plugin_main.cpp`, `plugin.json` | ✅ сделано |

**Хранение секретов (решение пользователя — вариант A):** логин/Application
Password WordPress пишутся в `<data_dir>/news_rewriter/.env` (ключи
`NEWS_REWRITER_WP_USERNAME` / `NEWS_REWRITER_WP_APP_PASSWORD`), вне `settings.ini`.
`WordPressSink` читает их из `.env` (с fallback на `params` для тестов/обратной
совместимости). В UI поля логина/пароля не попадают в сохраняемый JSON-конфиг.

**Текущая точка:** реализация завершена (включая расширение таксономии 7.5), тесты
зелёные (125). Если сессия прервана — запусти `./build.sh --tests`; при падении теста
WP см. `tests/test_sink_wordpress.cpp` и `src/sink_wordpress.cpp`. Не реализовано из
плана: полная Markdown-поддержка (6.x, отмечено как отдельная задача) и ручное
E2E-тестирование против живого WP-хоста (сделано пользователем: gsemantic.ru, тип
`posts`, Application Password — работает).

---

## 1. Как WordPress принимает статьи

WP REST API (версии ≥ 4.7) публикует записи через `POST {site}/wp-json/wp/v2/posts`.

- **Авторизация обязательна.** Без неё `/posts` возвращает `401`. Для приложения
  (сервисного аккаунта) стандартный путь — **Application Password**:
  в админке WP (`Профиль пользователя → Application Passwords`) генерируется
  пароль вида `xxxx xxxx xxxx xxxx`, который шлётся как HTTP Basic:
  `Authorization: Basic base64("{user}:{app_password}")`. Альтернатива — JWT-плагин
  (`Authorization: Bearer <jwt>`), но он требует установленного плагина на хосте;
  Basic/Application Password есть из коробки.
- **Тело запроса** — JSON со полями схемы записи WP:
  - `title` (строка) ← `title_rewritten`
  - `content` (HTML-строка) ← `body_rewritten` (преобразуем в HTML)
  - `status` (`draft` | `publish` | `pending` | `private`) — параметр конфига
  - `categories` (массив id), `tags` (массив id) — опционально из конфига
  - `excerpt` (строка) — опционально
  - `slug` (строка) — опционально (из url/title)
  - `author` (id) — опционально
  - `featured_media` (id) — опционально, требует предварительной загрузки картинки
- **Успех** — HTTP `201 Created`, тело содержит `{ "id": <post_id>, ... }`.
- **Ошибки** — `401` (неверный пароль/пользователь), `400` (невалидное тело),
  `403` (нет прав на `edit_posts`), `503` (хост лёг).

Важно: `content` в WP — это **HTML**, а `body_rewritten` у нас — обычный текст
(возможно с Markdown). Его надо превратить в HTML (абзацы по переносам строк,
экранирование `<>&"`, опц. простой Markdown → `<p>/<strong>/<em>`).

---

## 2. Файловая структура (изменения)

```
plugins/user_plugins/news_rewriter/
├── src/
│   ├── sink.h                 # добавить декларацию make_wordpress_sink
│   ├── sink_wordpress.cpp     # НОВЫЙ: WordPressSink (type "wordpress")
│   ├── http.h / http.cpp       # переиспользуем HttpClient::post (уже есть)
│   └── plugin_main.cpp        # зарегистрировать фабрику в SinkRegistry
└── tests/
    └── test_sink_wordpress.cpp # НОВЫЙ: тесты через MiniHttpServer
```

`Storage&` передаётся по контракту, но не используется для записи (аналогично
`HttpSink`).

---

## 3. Контракт модуля `WordPressSink`

Параметры берутся из `cfg.params` (`nlohmann_json_like`):

| Параметр | Обязательность | Описание |
|---|---|---|
| `site_url` | обязательный | базовый URL сайта, напр. `https://blog.example.com` (без `/wp-json/...`) |
| `username` | обязательный | WP-пользователь с правом `edit_posts` |
| `app_password` | обязательный | Application Password (с пробелами или без — нормализуем) |
| `status` | опционально | `draft` (по умолчанию) / `publish` / `pending` / `private` |
| `post_type` | опционально | тип записи WP: `posts` (по умолчанию) / `pages` / кастомный CPT (его `rest_base`, напр. `product`). Endpoint строится по slug |
| `categories` | опционально | массив числовых id категорий WP |
| `tags` | опционально | массив id тегов |
| `author` | опционально | id автора |
| `excerpt` | опционально | если пусто — WP сгенерирует сам |
| `featured_image` | опционально | URL картинки; если задан — грузим медиа (`/media`) и ставим `featured_media` |
| `timeout_seconds` | опционально | по умолч. 20 |
| `max_retries` | опционально | по умолч. 0 (сеть к WP может быть нестабильной — рекомендуем ≥ 2) |
| `retry_delay_ms` | опционально | по умолч. 1000 |

Алгоритм `write(const Article& article)`:

1. Валидация: `site_url`, `username`, `app_password`, непустые
   `title_rewritten`/`body_rewritten` (если рерайт пуст — `log_` + `return false`,
   чтобы worker пометил `Error`, а не слал пустоту).
 2. Формируем endpoint: `rtrim(site_url,'/') + "/wp-json/wp/v2/" + post_type`
    (параметр `post_type`, по умолчанию `"posts"`; для кастомных типов записи —
    их `rest_base`, напр. `"product"`; стандартные `"pages"` не принимают
    категории/теги — для них таксономия не шлётся).
3. Заголовки: `Content-Type: application/json`,
   `Authorization: Basic <base64(user:app_password)>`
   (пробелы в app_password удаляем перед кодированием).
4. Тело (собственный JSON, НЕ `article_to_json`):
   `{ "title":..., "content": <html>, "status":..., "categories":[...], ... }`.
5. `client_.post(endpoint, body, nc, headers)`; успех = `status==201`.
6. При сбое — ретраи с `retry_delay_ms`, как в `HttpSink`.
7. (опционально) если задан `featured_image` и пост создан (получили `id`):
   `POST /wp-json/wp/v2/media` с `Content-Type: image/...` + бинарём (загружаем
   через `HttpClient::post` с телом-файлом), получаем `media_id`, затем
   `POST /wp-json/wp/v2/media/<id>?post=<post_id>` или `PATCH /posts/<id>`
   с `featured_media`. Это точка роста этапа 7.2.

Конвертация `body_rewritten` → HTML: `html_escape` + разбиение по `\n\n` на
`<p>`, одинарные переносы → `<br>`; опц. элементарный Markdown
(`**x**`→`<strong>`, `*x*`→`<em>`, `# `→`<h2>`). Минимально — просто абзацы,
полная Markdown-поддержка — отдельная задача (6.x).

---

## 4. Регистрация (без правок ядра)

В `src/plugin_main.cpp`, рядом с `make_http_sink`:

```cpp
SinkRegistry::instance().register_factory("wordpress", make_wordpress_sink);
```

В `src/sink.h` добавить декларацию `make_wordpress_sink` (по образцу
`make_http_sink`). В `src/sink_wordpress.cpp` — реализация и фабрика
`std::unique_ptr<Sink> make_wordpress_sink(...)`.

Активный sink выбирается в конфиге плагина:

```json
"sink": {
  "type": "wordpress",
  "params": {
    "site_url": "https://blog.example.com",
    "username": "publisher",
    "app_password": "abcd efgh ijkl mnop",
    "status": "draft",
    "categories": [3],
    "tags": [7, 11],
    "max_retries": 3,
    "retry_delay_ms": 2000
  }
}
```

---

## 5. План задач

| # | Задача | Файлы | Критерий готовности |
|---|---|---|---|
| 7.1 | `WordPressSink`: POST в `/wp/v2/posts` с Basic-авторизацией и маппингом полей | `sink_wordpress.cpp`, `sink.h`, `plugin_main.cpp` | Статья появляется в WP (черновик/публикация); 401/403 обрабатываются понятной ошибкой; выбирается из конфига без правок ядра |
| 7.2 | `body_rewritten` → HTML (абзацы + экранирование) | `sink_wordpress.cpp` | Текст рерайта корректно отображается в редакторе WP, без сырых `<`/`&` |
| 7.3 | Загрузка `featured_media` (опц.) | `sink_wordpress.cpp`, `http.cpp` | Картинка по URL попадает в медиабиблиотеку и ставится обложкой поста |
| 7.4 | Тесты `test_sink_wordpress.cpp` | `tests/test_sink_wordpress.cpp` | MiniHttpServer эмулирует `/wp/v2/posts` (201, 401, 400, 503+ретраи); тесты зелёные |

---

## 6. Риски и открытые вопросы

- **HTTPS/сертификаты.** `HttpClient` (libcurl dlopen) уже умеет HTTPS; надо
  убедиться, что `CURLOPT_SSL_VERIFYPEER` включён (безопасность), и что на хосте
  нет самоподписанного серта (иначе — опция `insecure_ssl` в `NetworkConfig`).
- **Шаг экранирования app_password** — WP принимает с пробелами, но в Basic
  кодируем без пробелов; это нормализуем в коде.
- **Права пользователя.** Нужен WP-пользователь с `edit_posts` (роль
  `Author`/`Editor`/`Admin`). Для публикации сразу (`publish`) — `Editor`.
- **Дедупликация уже решена** в Storage (по `id`/`content_hash`) — повторный
  обход того же URL не отправит статью в WP второй раз.
- **Лимиты хостинга.** Некоторые шаред-хостинги ограничивают REST-частоту;
  `max_retries`/`retry_delay_ms` + backoff scheduler-а смягчают.
- **Альтернатива XML-RPC** (`wp.newPost`) — устарело и часто отключено; REST —
  предпочтительный путь, поэтому в плане только REST.

---

## 7. Критерий готовности

- В конфиге выбран `sink.type="wordpress"`; плагин по расписанию/вручную
  переписывает новость и публикует её на удалённом WP-хосте (черновиком или
  сразу), с корректным HTML и авторизацией.
- Сбой авторизации/сети не вешает очередь, логируется, уходит в retry по
  конфигу.
- Новый модуль добавлен без изменений fetcher/extractor/rewriter/worker.
