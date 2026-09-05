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
)

add_library(wp_coder_core STATIC ${CORE_SOURCES})
target_include_directories(wp_coder_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_ROOT}/include
)
set_target_properties(wp_coder_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(wp_coder_core PRIVATE -Wall -Wextra)

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

# --- Плагин (.so) ---
add_library(wp_coder SHARED
    ${MODULE_SOURCES}
    ${UI_SOURCES}
    src/plugin_main.cpp
)

target_include_directories(wp_coder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PROJECT_ROOT}/include
    ${PROJECT_ROOT}/external/imgui
    ${PROJECT_ROOT}/external/imgui/backends
    ${PROJECT_ROOT}/libs/headless_browser/include
)

target_compile_definitions(wp_coder PRIVATE
    WP_CODER_SKILLS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/skills")

target_link_libraries(wp_coder PRIVATE wp_coder_core)

# headless_browser (опционально).
if(NOT TARGET headless_browser)
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

# --- Тесты ---
option(WP_CODER_BUILD_TESTS "Build wp_coder unit tests" ON)
if(WP_CODER_BUILD_TESTS)
    add_executable(wp_coder_tests
        tests/test_main.cpp
        tests/test_tools_registry.cpp
        tests/test_skills_manager.cpp
        tests/test_engine.cpp
        tests/test_module_api.cpp
        tests/test_security.cpp
    )
    target_include_directories(wp_coder_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_ROOT}/include
    )
    target_link_libraries(wp_coder_tests PRIVATE wp_coder_core)
    target_compile_options(wp_coder_tests PRIVATE -Wall -Wextra)
    set_target_properties(wp_coder_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
endif()
