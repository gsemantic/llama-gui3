# Открытые дефекты хоста

Файл для дефектов, которые найдены, локализованы, но не исправлены в той
же сессии. Формат: симптом → воспроизведение → что установлено → корень →
варианты исправления. Один дефект — один раздел.

---

## OI-1. `PluginLoader` и `MenuRestore` падают с SIGSEGV в ctest

**Статус:** OPEN. **Найден:** 2026-11-16 (вместе с коммитами `e191f15` —
плагин 0.18.8–0.18.11 — и `1c7742d` — хостовые правки).

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
`test_plugin_loader` подсистема `window_coordinator` не задана (nullptr),
поэтому для `wp_coder` вызывается `ll_plugin_render()` →
`coder::ui::render_all_windows()`, которое делает `ImGui::Begin()/End()`
вне кадра. `ImGui::Begin` вне `NewFrame` — неопределённое поведение;
на практике это разыменование неготового состояния контекста → SIGSEGV.

В реальном приложении `NewFrame()` вызывается в главном цикле перед
`render_plugins()` (`src/ui/main_window.cpp:1628`), поэтому дефект виден
только в тестах.

### Варианты исправления

1. **Исправить тесты (предпочтительно, хостовое изменение).** Обрамить
   вызов `render_plugins()` в
   `ImGui::NewFrame(); manager.render_plugins(); ImGui::EndFrame();` в
   обоих тестах. Это делает тест честным: он эмулирует реальный кадр,
   как приложение. Минимальная и безопасная правка.
2. **Сделать `render_plugins()` устойчивым к отсутствию кадра.** В
   `PluginManager::render_plugins()` пропускать вызов `render_fn`, если
   кадр не начат. Требует доступа к признаку «кадр идёт»
   (`ImGuiContext::WithinFrameScope` — внутренний API; публичного
   `ImGui::IsFrameStarted()` в используемой версии может не быть).
   Риск: тест перестанет проверять рендер вообще — станет зелёным «на
   пустоте».
3. **Контракт плагина.** `ll_plugin_render` должен сам проверять, что
   кадр начат, и не рисовать иначе. Требует публичного признака из
   `imgui.h`; если его нет — заводить свой `g_frame_active` в
   `host_bridge/` по вызовам `NewFrame`/`EndFrame` хоста. Больше работы,
   чем вариант 1.

Рекомендуется вариант 1 как первый шаг (зелёный ctest без изменения
продуктового поведения), вариант 3 — как долгосрочный контракт, если
появится требование «плагин не падает вне кадра».

### Ссылки

- `tests/core/test_plugin_loader.cpp:97` — `manager.render_plugins()`.
- `tests/core/test_menu_restore.cpp:112` — `manager.render_plugins()`.
- `src/plugins/plugin_manager.cpp:1780` — `PluginManager::render_plugins()`.
- `src/plugins/plugin_manager.cpp:1837` — вызов `p->render_fn()`.
- `plugins/user_plugins/wp_coder/src/plugin_main.cpp:288` — `ll_plugin_render`.
- `src/ui/main_window.cpp:1628` — реальный цикл (кадр начат).
