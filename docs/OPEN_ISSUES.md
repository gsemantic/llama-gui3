# Открытые дефекты хоста

Файл для дефектов, которые найдены, локализованы, но не исправлены в той
же сессии. Формат: симптом → воспроизведение → что установлено → корень →
варианты исправления. Один дефект — один раздел.

---

## OI-1. `PluginLoader` и `MenuRestore` падают с SIGSEGV в ctest — RESOLVED

**Статус:** RESOLVED 2026-11-16 (правка тестов: координатор окон).
**Найден:** 2026-11-16 (вместе с коммитами `e191f15` — плагин
0.18.8–0.18.11 — и `1c7742d` — хостовые правки).

> **Итог.** Тесты дают `WindowCoordinator` в `PluginSubsystems` — как в
> реальном приложении. Тогда плагины с `render_fn` регистрируются в
> координаторе (`rendered_by_coordinator=true`), и
> `PluginManager::render_plugins()` **не** зовёт их `render_fn` напрямую:
> окна рисует координатор в `renderAll()` (а тесты его не зовут).
> Восстановление пунктов меню идёт в `render_plugins()` **до** проверки
> `rendered_by_coordinator`, поэтому `MenuRestore` остаётся валидным.
> `ctest`: 15/15 зелёные.
>
> **Почему не `NewFrame()/EndFrame()` (первая редакция, вариант 1).**
> `ImGui::NewFrame()` в тестовом контексте (без backend и без font atlas)
> сам падает SIGSEGV внутри `UpdateFontsNewFrame` → `GetDefaultFont`:
> `AddFontDefault()` не спасает, потому что при незаданном
> `ImGuiBackendFlags_RendererHasTextures` atlas блокируется и дефолтный
> шрифт не добавляется. Обернуть рендер в кадр без инициализации
> backend-а — не вариант.

### Симптом

`ctest` из `build/`:

```
87% tests passed, 2 tests failed out of 15
	14 - PluginLoader (SEGFAULT)
	15 - MenuRestore (SEGFAULT)
```

Остальные 13 тестов зелёные. Плагин-тесты (`wp_coder_tests`, 1458
проверок) зелёные.

### Воспроизведение

```sh
cd build && ctest --output-on-failure -R "PluginLoader|MenuRestore"
```

Стабильно: 5 прогонов подряд — 5 падений. При **прямом** запуске
`./build/tests/test_plugin_loader` с перенаправлением stdout в pipe/файл
дефект **нестабилен** (часто `rc=0`), то есть зависит от окружения
запуска — но через `ctest` воспроизводится всегда.

### Что установлено (бисекция и эксперименты)

1. **Не хостовые правки коммита `1c7742d`.** Дефект воспроизводится и
   при полностью откаченных хостовых правках.
2. **Не регрессия коммита `e191f15`.** Дефект воспроизводится и с
   плагином 0.18.7 (`aa96a02`) — то есть существовал раньше.
3. **Не worker-поток движка.** Временное отключение
   `coder::engine().start()` в `ll_plugin_init`
   (`plugins/user_plugins/wp_coder/src/plugin_main.cpp:283`) **не
   убирает** SIGSEGV.
4. **Корень — `ll_plugin_render()` без начатого ImGui-кадра.** Временное
   отключение `coder::ui::render_all_windows()` в `ll_plugin_render`
   (`src/plugin_main.cpp:288-291`) делает ctest **100% зелёным** (3/3).

### Корень

Тесты `tests/core/test_plugin_loader.cpp` (строка ~97) и
`tests/core/test_menu_restore.cpp` (строка ~112) вызывают
`manager.render_plugins()` **без** `ImGui::NewFrame()` / `ImGui::EndFrame()`.
`ImGui::CreateContext()` создан, но кадр не начат.

`PluginManager::render_plugins()` (`src/plugins/plugin_manager.cpp:1780`)
для плагинов без `rendered_by_coordinator` вызывает `p->render_fn()`. В
тестах подсистема `window_coordinator` не была задана (nullptr), поэтому
для `wp_coder` вызывался `ll_plugin_render()` →
`coder::ui::render_all_windows()` → `ImGui::Begin()/End()` вне кадра.
`ImGui::Begin` вне `NewFrame` — неопределённое поведение; на практике
разименование неготового состояния контекста → SIGSEGV.

В реальном приложении `render_plugins()` зовётся после `NewFrame()`
(`src/ui/main_window.cpp:1628`), а окна плагинов рисует `WindowCoordinator`
в `renderAll()`; `render_fn` из `render_plugins()` для них не зовётся.
Тесты это окружение не воспроизводили.

### Исправление (принято)

Тестам добавлен `ui::WindowCoordinator` в `PluginSubsystems` — как в
приложении. Плагины с `render_fn` регистрируются в нём
(`rendered_by_coordinator=true`), `render_plugins()` их `render_fn` не
зовёт, окна рисует координатор (в тестах `renderAll()` не зовётся).
Блок восстановления меню выполняется до проверки
`rendered_by_coordinator`, поэтому `MenuRestore` остаётся валидным.

### Отклонённые варианты

1. **`NewFrame()/EndFrame()` вокруг `render_plugins()`.** Не работает в
   тестовом контексте: `ImGui::NewFrame()` без backend (не задан
   `ImGuiBackendFlags_RendererHasTextures`, нет font atlas) падает
   SIGSEGV в `UpdateFontsNewFrame → GetDefaultFont`; `AddFontDefault()`
   не спасает, потому что atlas блокируется и дефолтный шрифт не
   добавляется.
2. **Пропуск `render_fn` при отсутствии кадра в `render_plugins()`.**
   Требует внутреннего признака `ImGuiContext::WithinFrameScope` или
   своего `g_frame_active`; тест перестал бы проверять рендер — зелёный
   «на пустоте».
3. **Контракт плагина (`ll_plugin_render` проверяет кадр).** Требует
   публичного признака из `imgui.h`; в реальном приложении кадр всегда
   начат, так что это долг, а не дефект. Оставлено на будущее, если
   появится требование «плагин не падает вне кадра».

### Ссылки

- `tests/core/test_plugin_loader.cpp:97` — `manager.render_plugins()`.
- `tests/core/test_menu_restore.cpp:112` — `manager.render_plugins()`.
- `src/plugins/plugin_manager.cpp:1780` — `PluginManager::render_plugins()`.
- `src/plugins/plugin_manager.cpp:1837` — вызов `p->render_fn()`.
- `plugins/user_plugins/wp_coder/src/plugin_main.cpp:288` — `ll_plugin_render`.
- `src/ui/main_window.cpp:1628` — реальный цикл (кадр начат).

---

## OI-2. `MenuRestore` падает с SIGFPE (деление на ноль) при загрузке `calculator`

**Статус:** открыт, не связан с RAG (предсуществующий).
**Найден:** 2026-11-24, в ходе R0.1 (план RAG-SPEC).

### Симптом

`ctest` из `build/`:

```
94% tests passed, 1 tests failed out of 18
	18 - MenuRestore (NUMERICAL)
```

Прямой запуск даёт `Исключение в операции с плавающей точкой`, rc=136
(SIGFPE). Последняя строка вывода — `calculator: инициализация`, то есть
падение происходит внутри `ll_plugin_init` плагина `calculator` или сразу
после неё.

### Воспроизведение

```sh
cd build && make test_menu_restore -j$(nproc)
./tests/test_menu_restore; echo $?   # 136
```

### Что установлено

* Воспроизводится на **чистом дереве** (проверено через
  `git stash push src/core/rag_manager_embedding.cpp
  src/core/embedding_generator.cpp tests/CMakeLists.txt` → пересборка →
  тот же SIGFPE). Правки R0.1 к падению отношения не имеют.
* OI-1 (SIGSEGV в `MenuRestore`) закрыт и к этому не относится: здесь
  другой сигнал (SIGFPE, не SIGSEGV) и другой момент (init плагина
  `calculator`, а не `render_plugins`).
* `test_plugin_loader` с тем же плагином проходит (rc=0) — значит дело не
  только в загрузке `calculator`, а в пути `MenuRestore`.

### Корень

Не установлен. Кандидаты: целочисленное деление на ноль в коде плагина
`calculator` (`plugins/examples/calculator/`) или в восстановлении меню
`MenuRestore`, куда загружается этот плагин.

### Что дальше

Не блокирует RAG-SPEC. При следующем касании `MenuRestore`/`calculator`
добавить отладочный вывод или прогнать под gdb.
