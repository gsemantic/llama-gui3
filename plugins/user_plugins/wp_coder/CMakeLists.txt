# wp_coder — AI-кодер с модульной архитектурой.
# WordPress — один из модулей. Другие модули подключаются аналогично.
#
# Структура:
#   core/       — универсальное ядро (движок, реестр инструментов, навыки)
#   modules/    — доменные модули (wordpress, python, ...)
#   ui/         — модульный интерфейс (окна, настройки)
#   src/        — точка входа плагина (plugin_main)

cmake_minimum_required(VERSION 3.14)
project(wp_coder CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

# --- Исходники ядра ---
set(CORE_SOURCES
    core/engine.cpp
    core/tools_registry.cpp
    core/skills_manager.cpp
    core/module_api.cpp
    core/base_tools.cpp
    core/git_tools.cpp
    core/project.cpp
)

# --- Исходники модулей ---
set(MODULE_SOURCES
    modules/wordpress/wp_module.cpp
    modules/wordpress/wp_tools.cpp
    modules/python/python_module.cpp
    modules/python/python_tools.cpp
    modules/devops/devops_module.cpp
    modules/devops/devops_tools.cpp
)

# --- Исходники UI ---
set(UI_SOURCES
    ui/coder_window.cpp
)

# --- Точка входа плагина ---
set(PLUGIN_MAIN
    src/plugin_main.cpp
)

add_library(wp_coder SHARED
    ${CORE_SOURCES}
    ${MODULE_SOURCES}
    ${UI_SOURCES}
    ${PLUGIN_MAIN}
)

target_include_directories(wp_coder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PROJECT_ROOT}/include
    ${PROJECT_ROOT}/external/imgui
    ${PROJECT_ROOT}/external/imgui/backends
    ${PROJECT_ROOT}/libs/headless_browser/include
)

# Каталог навыков.
target_compile_definitions(wp_coder PRIVATE
    WP_CODER_SKILLS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/skills")

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
