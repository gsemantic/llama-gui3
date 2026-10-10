# wp_coder — AI-кодер с модульной архитектурой.
#
# Структура:
#   core/       — универсальное ядро (движок, реестр инструментов, навыки)
#   modules/    — доменные модули (wordpress, python, devops)
#   ui/         — модульный интерфейс (окна, настройки)
#   src/        — точка входа плагина (plugin_main)

cmake_minimum_required(VERSION 3.14)
project(wp_coder CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

# --- Ядро (статическая библиотека — переиспользуется плагином и тестами) ---
set(CORE_SOURCES
    core/engine.cpp
    core/tools_registry.cpp
    core/skills_manager.cpp
    core/module_api.cpp
    core/base_tools.cpp
    core/git_tools.cpp
    core/project.cpp
    core/security.cpp
    core/command_policy.cpp
    core/json.cpp
    core/tool.cpp
    core/permission.cpp
    core/permission_engine.cpp
    core/tool_protocol.cpp
    core/glob.cpp
    core/text_edit.cpp
    core/file_lock.cpp
    core/apply_patch.cpp
    core/agent_components.cpp
    core/llm_event.cpp
    core/llm_source.cpp
    core/llm_client.cpp
    core/message.cpp
    core/id_prefix.cpp
    core/session_store.cpp
    core/compaction.cpp
    core/agent_registry.cpp
    core/subagent.cpp
    core/instruction.cpp
    core/harness_profile.cpp
    core/snapshot.cpp
    core/diff.cpp
    core/timeline.cpp
    core/tool_display.cpp
    core/stream_panel.cpp
)

add_library(wp_coder_core STATIC ${CORE_SOURCES})
target_include_directories(wp_coder_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_ROOT}/include
)
set_target_properties(wp_coder_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(wp_coder_core PRIVATE -Wall -Wextra)
find_package(Threads REQUIRED)
target_link_libraries(wp_coder_core PUBLIC Threads::Threads)

# --- Модули ---
set(MODULE_SOURCES
    modules/wordpress/wp_module.cpp
    modules/wordpress/wp_tools.cpp
    modules/python/python_module.cpp
    modules/python/python_tools.cpp
    modules/devops/devops_module.cpp
    modules/devops/devops_tools.cpp
)

# --- UI ---
set(UI_SOURCES
    ui/coder_window.cpp
)

# --- Мост к ABI хоста (host_bridge) ---
# Единственное место в плагине, которому законно знать про LlamaHostApi.
# Лежит отдельно от src/ (там только точка входа, правило из И0.9) и
# отдельно от core/ (D-7: ядро от хоста не зависит). Проверяется тестом
# test_manifest_consistency.cpp: core/ не включает plugins/plugin_api.h.
set(HOST_BRIDGE_SOURCES
    host_bridge/llm_stream_probe.cpp
    host_bridge/llm_stream_shim.cpp
    host_bridge/llm_blocking.cpp
)

# --- Плагин (.so) ---
add_library(wp_coder SHARED
    ${MODULE_SOURCES}
    ${UI_SOURCES}
    src/plugin_main.cpp
    ${HOST_BRIDGE_SOURCES}
)

target_include_directories(wp_coder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PROJECT_ROOT}/include
    ${PROJECT_ROOT}/external/imgui
    ${PROJECT_ROOT}/external/imgui/backends
    ${PROJECT_ROOT}/libs/headless_browser/include
)

# Версия плагина: единственный источник истины — plugin.json.
# Раньше версия была продублирована в четырёх местах и разошлась
# (0.3.0 / 0.1.0 / 0.3.0 / 0.4.0) — см. И0.8 плана AGENT_PARITY_PLAN.md.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json" WP_CODER_PLUGIN_JSON)
string(REGEX MATCH "\"version\"[ ]*:[ ]*\"([^\"]+)\""
       WP_CODER_VERSION_MATCH "${WP_CODER_PLUGIN_JSON}")
# CMAKE_CMATCH_* живёт только до следующей операции со строкой — забираем сразу.
set(WP_CODER_VERSION "${CMAKE_MATCH_1}")
if(NOT WP_CODER_VERSION)
    message(FATAL_ERROR
        "wp_coder: не удалось прочитать version из plugin.json")
endif()
message(STATUS "wp_coder: version ${WP_CODER_VERSION} (plugin.json)")

target_compile_definitions(wp_coder PRIVATE
    WP_CODER_SKILLS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/skills"
    # И9.7: каталог ПОСТАВЛЕННЫХ профилей harness. Тот же приём, что с
    # навыками: дерево плагина знает только этот файл (src/), а ядро
    # получает готовую строку и о своём расположении не знает.
    WP_CODER_PROFILES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/profiles/wp_coder"
    WP_CODER_VERSION="${WP_CODER_VERSION}")

target_link_libraries(wp_coder PRIVATE wp_coder_core)

# headless_browser: в составе корневого проекта цель уже определена
# (add_subdirectory(libs/headless_browser)) — линкуем её напрямую.
if(TARGET headless_browser)
    target_link_libraries(wp_coder PRIVATE headless_browser)
else()
    # Автономная сборка плагина: ищем готовую статическую библиотеку.
    add_library(headless_browser STATIC IMPORTED)
    find_library(HEADLESS_BROWSER_LIB NAMES headless_browser
        PATHS ${PROJECT_ROOT}/build/libs/headless_browser
              ${PROJECT_ROOT}/libs/headless_browser
        NO_DEFAULT_PATH)
    if(HEADLESS_BROWSER_LIB)
        set_target_properties(headless_browser PROPERTIES
            IMPORTED_LOCATION ${HEADLESS_BROWSER_LIB}
            INTERFACE_INCLUDE_DIRECTORIES ${PROJECT_ROOT}/libs/headless_browser/include)
        target_link_libraries(wp_coder PRIVATE headless_browser)
    endif()
endif()

set_target_properties(wp_coder PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    POSITION_INDEPENDENT_CODE ON
)
target_compile_options(wp_coder PRIVATE -Wall -Wextra)

# Манифест кладём рядом с .so ПОД ИМЕНЕ ПЛАГИНА.
#
# PluginManager::find_manifest_path() (src/plugins/plugin_manager.cpp)
# пробует кандидатов по порядку: lib<name>.json, <name>.json,
# <name>.plugin.json и только потом общий plugin.json. И0.8 копировал
# сюда plugin.json — это был кандидат №4, недостижимый из-за №2,
# поэтому хост продолжал читать бросившийся в build/plugins/
# wp_coder.json версии 0.1.0 (D20 на самом деле остался открытым).
#
# Имя обязательно своё: общий plugin.json в каталоге с несколькими
# плагинами подхватывается всеми, у кого нет манифеста с именем, —
# так hello_plugin получал capabilities и версию wp_coder.
# Ниже же общий файл удаляется, чтобы старые копии не оставались.
add_custom_command(TARGET wp_coder POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
        "${CMAKE_BINARY_DIR}/plugins/wp_coder.json"
    COMMAND ${CMAKE_COMMAND} -E rm -f
        "${CMAKE_BINARY_DIR}/plugins/plugin.json"
    COMMENT "wp_coder: манифест в build/plugins/wp_coder.json (без общего plugin.json)"
)

# --- Тесты ---
option(WP_CODER_BUILD_TESTS "Build wp_coder unit tests" ON)
if(WP_CODER_BUILD_TESTS)
    add_executable(wp_coder_tests
        tests/test_main.cpp
        tests/test_json.cpp
        tests/test_tool.cpp
        tests/test_permission.cpp
        tests/test_permission_engine.cpp
        tests/test_tools_registry.cpp
        tests/test_skills_manager.cpp
        tests/test_engine.cpp
        tests/test_module_api.cpp
        tests/test_security.cpp
        tests/test_command_policy.cpp
        tests/test_glob.cpp
        tests/test_apply_patch.cpp
        tests/test_edit_cascade.cpp
        tests/test_file_lock.cpp
        tests/test_todo.cpp
        tests/test_read_tool.cpp
        tests/test_bash_tool.cpp
        tests/test_output_truncation.cpp
        tests/test_llm_events.cpp
        tests/test_llm_source.cpp
        tests/test_llm_client.cpp
        tests/test_llm_stream_abi.cpp
        tests/test_llm_blocking.cpp
        tests/test_shell_cancel.cpp
        tests/test_terminal_outcomes.cpp
        tests/test_paths_parity.cpp
        ${HOST_BRIDGE_SOURCES}
        tests/test_message.cpp
        tests/test_session_store.cpp
        tests/test_compaction.cpp
        tests/test_agent_registry.cpp
        tests/test_agent_config.cpp
        tests/test_instruction.cpp
        tests/test_iteration_iv_invariants.cpp
        tests/test_agent_loop.cpp
        tests/test_base_tools.cpp
        tests/test_wp_tools.cpp
        # ПОРЯДОК ФАЙЛОВ ЗНАЧИТ, И ЭТО НЕ КОСМЕТИКА (И8.15).
        # test_iteration_iv_invariants.cpp проверяет, что КАЖДЫЙ пишущий
        # инструмент классифицирован, и проходит, только пока модульные
        # инструменты ещё НЕ зарегистрированы: wp_create_site помечен
        # TF_WRITES_FILES и в таблицу проверки не входит намеренно. Любой
        # файл, регистрирующий wp/python/devops-инструменты, обязан стоять
        # ПОСЛЕ него — иначе проверка падает.
        # Следствие: test_task_tool.cpp перенесён ниже test_wp_tools.cpp,
        # потому что его проверки И8.15 смотрят каталог субагента и должны
        # видеть deploy/wp_db: без них «у wp_theme нет deploy» прошло бы на
        # отсутствии инструмента вообще, то есть проверяло бы пустоту.
        tests/test_task_tool.cpp
        tests/test_python_tools.cpp
        tests/test_devops_tools.cpp
        # И9.7: профили harness. После файлов, регистрирующих
        # инструменты, — по двум причинам, и обе существенные.
        # (1) Как и у соседей выше: test_iteration_iv_invariants.cpp
        # проходит, только пока модульные инструменты не зарегистрированы.
        # (2) СВОЯ: проверка запретов профиля сверяет ПОЛНОТУ по ключам
        # живого реестра (новый ключ обязан быть либо разрешён, либо
        # запрещён) — на неполном реестре она прошла бы на его неполноте.
        tests/test_harness_profile.cpp
        # И11.4: виджет вызова по типу инструмента. Инструментов НЕ
        # регистрирует сам, но сверяет ПОЛНОТУ своего перечня с живым
        # реестром — поэтому обязан стоять после файлов, регистрирующих
        # модульные инструменты (test_wp_tools.cpp, test_task_tool.cpp,
        # test_python_tools.cpp, test_devops_tools.cpp), иначе сверка шла бы
        # по неполному реестру и прошла бы на его неполноте. То же основание,
        # что у test_harness_profile.cpp выше.
        tests/test_tool_display.cpp
        # И10.1: снимки рабочего каталога. Инструментов не регистрирует,
        # поэтому может стоять где угодно до test_iteration_iv_invariants.cpp;
        # стоит после файлов, зовущих цикл агента (test_agent_loop.cpp),
        # потому что проверяет состояние движка — то, что цикл меняет.
        tests/test_snapshot.cpp
        # И10.5: diff для показа в UI. Инструментов не регистрирует и
        # состояние движка не трогает, поэтому может стоять где угодно до
        # test_iteration_iv_invariants.cpp; стоит после test_snapshot.cpp,
        # потому что проверяет то, чем снимок кормит.
        tests/test_diff.cpp
        # И11.1: строки показа diff (разбор патча, trim_diff, вид строки).
        # Инструментов не регистрирует и состояние движка не трогает,
        # поэтому может стоять где угодно до
        # test_iteration_iv_invariants.cpp; стоит после test_diff.cpp,
        # потому что читает его патчи.
        tests/test_diff_view.cpp
        # И11.2: diff вызова (`metadata.filediff`) и подсветка «до/после».
        # Инструментов не регистрирует (только зовёт зарегистрированные), но
        # проверяет, что ПИШУЩИЕ инструменты отдают diff, поэтому стоит после
        # test_base_tools.cpp и до регистрации модульных инструментов.
        tests/test_diff_tool.cpp
        # И11.3: дерево вызовов инструментов по ходам. Инструментов не
        # регистрирует и состояние движка не трогает (работает на
        # сообщениях, собранных вручную), поэтому может стоять где угодно до
        # test_iteration_iv_invariants.cpp; стоит после test_message.cpp,
        # потому что длительность вызова живёт в модели сообщений.
        tests/test_timeline.cpp
        # И11.6: панель стриминга — что видно, пока ход не закрыт.
        # Инструментов не регистрирует и состояние движка не трогает
        # (работает на буфере, собранном вручную), поэтому стоит после
        # test_timeline.cpp: обе панели показывают работу агента, и
        # граница между ними («таймлайн — закрытые ходы, панель — текущий»)
        # проверяется на одном и том же материале.
        tests/test_stream_panel.cpp
        tests/test_manifest_consistency.cpp
        tests/test_plan_consistency.cpp
        tests/test_grammars_lock.cpp
        ${MODULE_SOURCES}
    )
    target_include_directories(wp_coder_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/host_bridge
        ${PROJECT_ROOT}/include
        ${PROJECT_ROOT}/libs/headless_browser/include
    )
    target_link_libraries(wp_coder_tests PRIVATE wp_coder_core Threads::Threads)
    target_compile_definitions(wp_coder_tests PRIVATE
        WP_CODER_VERSION="${WP_CODER_VERSION}"
        # И9.7: тот же путь, что у плагина. Проверка «поставленные профили
        # читаются» обязана смотреть на НАСТОЯЩИЕ файлы, а на свою копию:
        # копия прошла бы, пока файлы в дереве плагина сломаны.
        WP_CODER_PROFILES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/profiles/wp_coder")
    # Модули линкуются с headless_browser (wp_tools.cpp использует render_dom).
    if(TARGET headless_browser)
        target_link_libraries(wp_coder_tests PRIVATE headless_browser)
    endif()
    target_compile_options(wp_coder_tests PRIVATE -Wall -Wextra)
    set_target_properties(wp_coder_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    # Каталог, куда POST_BUILD кладёт .so и манифест. Тесты проверяют
    # манифест по месту: тест на исходнике CMakeLists ловит лишь
    # намерение, а этот — результат. Тесты собирают исходники сами и
    # не линкуются с библиотекой wp_coder, поэтому зависимость явная —
    # иначе манифеста в каталоге просто не будет.
    add_dependencies(wp_coder_tests wp_coder)
    target_compile_definitions(wp_coder_tests PRIVATE
        WP_CODER_BUILD_PLUGIN_DIR="${CMAKE_BINARY_DIR}/plugins"
    )
endif()
