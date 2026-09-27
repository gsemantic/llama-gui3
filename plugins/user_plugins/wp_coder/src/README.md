# src/ — точка входа плагина

Здесь лежит **ровно один** файл: `plugin_main.cpp`.

## Что здесь было раньше (и почему удалено)

До И0.9 (2026-09-25) в каталоге лежали 17 файлов параллельной системы
агентов: `wp_coder_orchestrator.{h,cpp}`, `wp_coder_plugin.cpp` и семь
`wp_*_agent.{h,cpp}` (theme / plugin / hook / deploy / rag / terminal / file).

Эта система:

- **не была включена ни в один список исходников** `CMakeLists.txt` — то есть
  не компилировалась вообще (проверено по `nm` по `plugins/libwp_coder.so`:
  символов `plugin_get_exports` / `plugin_create_agent` в бинарнике нет);
- содержала 47 заглушек вида `{"status", "stub"}` и 14 строк
  `"not implemented yet"`;
- ссылалась на ABI, которого нет в проекте: `AGENT_PLUGIN_API_VERSION`,
  `PluginExports`, `plugin_create_agent_fn` (`wp_coder_plugin.cpp`),
  а также на `agents::AgentContextMock` и `<gtest/gtest.h>` в тестах.

Из-за неё `plugins/wp_coder_refactor_plan.md` объявлял готовность 100 %,
хотя описывал именно мёртвый код.

Работающая система — `core/` (ReAct-движок) + `modules/` (доменные модули).
История удалённого кода доступна в git:

```sh
git log --diff-filter=D -- plugins/user_plugins/wp_coder/src/wp_theme_agent.cpp
git show HEAD~1:plugins/user_plugins/wp_coder/src/wp_file_agent.cpp
```

## Правило

Не добавлять в `src/` агентов, не добавив их в:

1. `CORE_SOURCES` / `MODULE_SOURCES` / `UI_SOURCES` в `../CMakeLists.txt`;
2. список тестов `wp_coder_tests` в том же `../CMakeLists.txt`;
3. список тестов в плане [`../AGENT_PARITY_PLAN.md`](../AGENT_PARITY_PLAN.md).

Файл, который не компилируется и не покрыт тестами, — это не код, а шум:
он вводит в заблуждение и прячет реальные пробелы.

Субагенты (итерация **И8** плана) будут жить в `core/agent_registry.*` и
опираться на `core/agent_components.*` — там уже есть цикл, разрешения и
работа с сессией. Переносить туда логику из удалённых файлов не нужно:
настоящие `wp_theme` / `wp_plugin` / `wp_hook` / `wp_deploy` пишутся заново
как инструменты + промпты (И8.15).

## Исключение: `host_bridge/` (И6.4)

Плагину законно знать про `LlamaHostApi` в двух местах:

- **точка входа** `src/plugin_main.cpp` — получает `api` в `ll_plugin_init`;
- **`host_bridge/`** — мост к ABI: проба наличия полей (`offsetof` + `size`)
  и проброс колбэков C в `std::function`.

`host_bridge/` — не точка входа, поэтому в `src/` он не лежит: здесь правило
«только `plugin_main.cpp`» проверяется тестом и ослаблять его нельзя (именно
оно в И0.9 не дало вернуться мёртвой параллельной системе агентов).

При этом `core/` и `modules/` про ABI хоста знать не должны (D-7): там живут
только `std::function`-колбэки. Проверяется тестом
`core_does_not_depend_on_the_host_abi` — и исключение у него ровно одно,
названное: `host_bridge/`.
