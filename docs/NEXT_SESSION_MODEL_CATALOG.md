# Задача для новой сессии: хост-каталог моделей (models.dev) + поля ABI — шаг 1

> **Статус: шаги 1–3 ВЫПОЛНЕНЫ** (2026-11-16, плагин `0.18.13`). Ниже —
> исходная постановка; что реально сделано — разделы «Итог шага 1/2/3».
>
> Напоминалка для следующей сессии. Читать в первую очередь. Актуальна на
> коммит `88d48a5` (середина 2026-11-16).

## Контекст

Репозиторий `/home/alex/projects/llama-gui`, ветка `main` синхронна с `origin`
на коммите `88d48a5`. Только что закрыты три вещи (уже в истории):

- `e191f15` — плагин wp_coder 0.18.8–0.18.11;
- `1c7742d` — хостовые правки (reasoning-доставка, активная беседа по id,
  crash handler, markdown-подсветка; версия `0.1.96`);
- `c8167e9` + `88d48a5` — задача OI-1 и её закрытие (`docs/OPEN_ISSUES.md`).

Некоммиченным в дереве остаётся **только** `profiles/glm 4.7-flash.json`
(рабочий профиль пользователя) — **не коммитить, не откатывать**.

Проверки на момент старта: `ctest` в `build/` — 15/15; плагин-тесты
(`/tmp/opencode/wp_coder_build`, `-DCMAKE_BUILD_TYPE=Release`) — 1458/0.

## Исходная цель (почему это нужно)

Плагин wp_coder не знает, какая модель реально обслуживает запрос, — через ABI
её не спросить. Отсюда:

- лимиты модели берутся из ручных настроек `wp_coder.context_limit`;
  при `0` `compaction::usable()==0` → `is_overflow()` всегда `false` → история
  **не сжимается никогда** (`core/engine.cpp`, `core/compaction.cpp`);
- цены — из `wp_coder.price_input_per_1m` (иначе «цена не задана»);
- нативный вызов определяется пробой (`NativeToolSupport` — одно глобальное
  состояние, не по модели) (`core/engine.cpp`);
- семейство модели угадывается по frontmatter агента, а не по факту
  (`core/prompts.h`); env-блок намеренно не называет модель (отклонение 97).

Каталог `models.dev` (его держит локальный клиент opencode) содержит всё это:
`tool_call`, `reasoning`, `limit.context/output`, `cost.input/output/cache_read/
cache_write`, `family`, `canonical_model_id`, `status`.

## Что сделать (шаг 1 — ТОЛЬКО ХОСТ; плагин не трогать)

1. **`src/core/model_catalog.{h,cpp}`** — новый модуль:
   - источник: HTTP
     `https://models.opencode.ai/experimental/v2.0/api.json?type=chat,decision,image`
     (11.1 МБ raw / 832 КБ gzip; поддерживает `If-None-Match` → 304). Резерв —
     кэш opencode в `~/.local/share/opencode/opencode.db`, таблица `kv`, ключ
     `models-dev:v2:catalog` (`{"updatedAt","digest","body"}`), но **SQLite dev-
     заголовков в системе нет** — читать БД опционально, через sqlite3 CLI или
     отдельной задачей; основной путь — HTTP.
   - кэш на диск: `~/.llama-gui/models_dev.json` + etag; проекция только нужных
     полей (2.2 МБ raw / ~188 КБ gzip).
   - фоновый refresh по образцу `OpenRouterClient::get_models_async`
     (`src/core/openrouter_client.cpp`).
   - **сопоставление модели с каталогом** по `endpoint_url`, а `provider_name`
     хоста НЕ равен имени в каталоге (у хоста «Qwen (DashScope)», в каталоге
     `alibaba`). Ключ — `api`-URL провайдера. Порядок:
     (1) exact `(провайдер по endpoint, model_id)`; (2) `canonical_model_id` по
     суффиксу с приоритетом провайдера из (1); (3) любой провайдер с тем же
     `canonical_model_id`; (4) нормализованное совпадение имени.
   - **если запись найдена у чужого провайдера — помечать «справочно»** (цены —
     ориентир, не счёт).
   - возвращать: `family`, `tools_supported`, `reasoning`, `context`,
     `max_output`, `price_in`, `price_out`, `cache_read`, `cache_write`,
     `provider`, `source` (exact/reference/unknown).

2. **`include/plugins/plugin_api.h`** — два поля **в конец** `LlamaHostApi`
   (после `llm_chat_cancel`), append-only, с doc:
   ```c
   char* (*model_info)(LlamaPluginHost* host, const char* model_id); /* JSON */
   char* (*model_current)(LlamaPluginHost* host);                    /* JSON: local|cloud */
   ```
   Строки — malloc, плагин освобождает `free_string`.

3. **`src/plugins/plugin_manager.cpp`** — реализация обоих полей в
   `host_api_table()`; хост берёт модель так: облако —
   `settings->cloud_provider().model_id` + `.endpoint_url`
   (`include/core/settings.h`), локально — `settings->get_model_path()`
   (`src/core/settings_model.cpp`). Для локальных возвращать `source:"local"`
   и подсказки из каталога (если нашлись), помня, что локальных GGUF в каталоге
   обычно нет.

4. **`tests/core/test_llm_stream_bridge.cpp`** — расширить append-only assert
   (`test_new_host_api_fields_are_append_only`): новые поля идут в конец, между
   полями ничего не вставлено. При необходимости — тест самого `model_catalog`
   (разбор проекции, сопоставление по endpoint).

## Жёсткие ограничения проекта

- **ABI append-only.** Поле только в конец `LlamaHostApi`; плагин проверяет
  наличие `offsetof` до вызова (`host_bridge/llm_stream_probe.cpp` — образец).
  Поле в середине → старый `.so` читает мусор и падает без сообщения.
- **Старый плагин должен продолжать работать**: он новых полей не трогает
  (его `offsetof`-проба их не читает).
- `profiles/glm 4.7-flash.json` — **не коммитить**.
- Версия — на границе итерации; сейчас `0.1.96` (`CMakeLists.txt`,
  `VERSION_PATCH`), следующая `0.1.97`.
- После правки `CMakeLists.txt`/`tests/CMakeLists.txt` — `cmake .` в `build/`,
  иначе тесты соберутся со старыми определениями.
- Мутационный прогон для новых guard-тестов (правило 13 `SESSION_START.md`
  плагина).
- Хостовую сборку/тесты гонять только если трогал хост/ABI — здесь именно так.

## Критерии готовности шага 1

- Хост собирается; `ctest` в `build/` зелёный (15/15 + новые).
- Два поля в конце `LlamaHostApi`; append-only assert расширен и зелёный.
- `model_catalog` грузит каталог, кэширует с etag, сопоставляет по
  endpoint+id, отдаёт проекцию; при недоступности сети — работает из кэша.
- Старый `libwp_coder.so` (0.18.11) загружается и `PluginLoader`/`MenuRestore`
  остаются зелёными.

## Итог шага 1 (2026-11-16, 0.1.97)

Сделано:

- `include/core/model_catalog.h` + `src/core/model_catalog.cpp`:
  - HTTP `api.json` (gzip, `If-None-Match` → 304), кэш
    `~/.llama-gui/models_dev.json` + `models_dev.etag`;
  - проекция полей → `CatalogModel` (provider, canonical_id, family,
    tools/reasoning, context/output, цены, base_url);
  - сопоставление по endpoint: exact → canonical-суффикс → любой провайдер
    (reference) → нормализованное имя; локально — по имени GGUF (`local`);
  - `ensure_loaded()` грузит кэш и запускает фоновый refresh; вызывается из
    `MainWindow::initializePlugins`, чтобы каталог был тёплым к первому ходу.
- ABI: `model_info` и `model_current` в КОНЕЦ `LlamaHostApi`
  (`include/plugins/plugin_api.h`), реализация в `plugin_manager.cpp`.
- Тесты: `tests/core/test_model_catalog.cpp` (8 проверок разбора/матчинга);
  append-only assert расширен на два новых поля.
- Мутационный прогон: вставка поля в середину ломает append-only assert;
  снятие условия endpoint в exact-матчинге ломает catalog-тесты.
- Сборка зелёная, `ctest` 16/16 (было 15).

Дальше (не в этом шаге): шаг 2 — замена `get_preset_price` на lookup по
каталогу; шаг 3 — плагин читает `model_info`/`model_current`.

## Итог шага 2 (2026-11-16, 0.1.99)

- В `CatalogModel`/`CatalogModelInfo` добавлен `has_cost`: цена `0` у
  бесплатной модели — настоящая цена, её нельзя отличить от «тарифа нет»
  по значениям. `has_cost` заполняется парсером и пишется в JSON/кэш.
- `ModelCatalog::lookup_price(endpoint, model_id, in, out)` — тариф по
  каталогу (exact или reference), но только если у записи есть тариф.
- `chat_message_send.cpp` и `cloud_services_dialog.cpp` при `auto_price`
  берут цену из каталога, а встроенная таблица `get_preset_price` осталась
  офлайн-подстраховкой (таблица НЕ удалена).
- Тесты: `has_cost` (бесплатная vs отсутствующая запись), мутационный
  прогон подтверждает guard; `ctest` 16/16.

Дальше: шаг 3 — плагин читает `model_info`/`model_current` (лимиты, цены,
семейство из каталога, отдельный тариф кэша).

## Итог шага 3 (2026-11-16, плагин 0.18.13)

- Проба ABI: `host::has_model_info` / `has_model_current` (offsetof+size);
  на старом хосте слоты пустые — поведение как до шага 3.
- `HostCallbacks::model_current` / `model_info`; в `plugin_main.cpp` они
  вызывают поля хоста и освобождают строку через `free_string`.
- `Engine::load_settings`: `model_current` читается один раз на задачу;
  `context`/`max_output`/цены из каталога используются ТОЛЬКО если
  настройка плагина не задана. Явная настройка важнее.
- Семейство промпта: если агент не объявил `model:`, берётся фактический
  `model_id`/`family` из каталога (`host_model_family`).
- Тесты: проба старых полей, приоритет настроек над каталогом, заполнение
  лимитов/цен из каталога, отсутствие слота = «не задано».
- `wp_coder_tests` — 1463/0; `ctest` — 16/16.

Отдельный тариф кэша (`cache_read`/`cache_write`) пока читается в JSON
каталога, но в расчёте стоимости плагина не участвует — это отдельная
задача панели стоимости.

## Что НЕ входит в шаг 1

- Плагин (чтение `model_info`/`model_current`, лимиты/цены/семейство из
  каталога, отдельный тариф кэша) — это шаг 3, отдельная задача.
- Замена `get_preset_price` (`include/core/openrouter_types.h`) на lookup по
  каталогу — шаг 2.

## Полезные факты из разведки

- `~/.local/share/opencode/opencode.db` → `kv` → `models-dev:v2:catalog`
  (`sha256(body)` == `digest`), мета — `models-dev:v2:catalog:meta` (`etag`).
- Каталог: 324 провайдера, ~9900 моделей, у 299 есть `api`.
- `endpoint_url` → провайдер: `dashscope-intl.../compatible-mode/v1`→`alibaba`,
  `openrouter.ai/api/v1`→`openrouter`, `127.0.0.1:1234/v1`→`lmstudio`,
  `opencode.ai/inference/openai/v1`→`opencode`.
- v2-поля модели: `capabilities.tools.supported`, `capabilities.reasoning`,
  `limit.context/output`, `cost.text.input.price` +
  `cost.text.input.cache.read/write`, `family`, `canonical_id`.
- v1-поля модели: `tool_call`, `reasoning`, `reasoning_options`,
  `limit.context/output`, `cost.input/output/cache_read/cache_write`,
  `family`, `canonical_model_id`, `open_weights`, `status`, `modalities`.
- `gdb` в системе нет; `libsqlite3.so.0` есть, но `sqlite3.h` (dev) — нет.
- `get_preset_price` (таблица ~40 моделей вручную) —
  `include/core/openrouter_types.h`; её заменит каталог (шаг 2).
