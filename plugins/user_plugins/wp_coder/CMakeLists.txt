# wp_coder — AI-кодер для WordPress (MVP-каркас).
# Собирается как .so, подключает только SDK хоста (include/plugins/plugin_api.h)
# и Dear ImGui (символы резолвятся из исполняемого файла приложения).
cmake_minimum_required(VERSION 3.14)
project(wp_coder CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

add_library(wp_coder SHARED
    src/plugin_main.cpp
    src/agent.cpp
    src/tools.cpp
    src/project.cpp
)

target_include_directories(wp_coder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PROJECT_ROOT}/include
    ${PROJECT_ROOT}/external/imgui
    ${PROJECT_ROOT}/external/imgui/backends
    ${PROJECT_ROOT}/libs/headless_browser/include
)

# Каталог поставляемых навыков (skills/*.md) для резолва из кода.
target_compile_definitions(wp_coder PRIVATE
    WP_CODER_SKILLS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/skills")

# headless_browser — берём уже собранную статическую либу из build/ (или libs/),
# как в news_rewriter. Символы не конфликтуют (это отдельная либа приложения).
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
    else()
        # Библиотека недоступна — headless_render вернёт ошибку доступности в рантайме.
        message(WARNING "wp_coder: headless_browser не найден, инструмент headless_render отключён")
    endif()
endif()

set_target_properties(wp_coder PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    POSITION_INDEPENDENT_CODE ON
)
