# news_rewriter — плагин-агент: обход адресов, рерайт новостей через LLM,
# локальное сохранение. Разрабатывается независимо от основного проекта.
#
# Структура:
#   news_rewriter_core (STATIC) — чистая логика (json, config, worker, ...),
#                                 без ImGui и без зависимости от хоста.
#   news_rewriter (SHARED)      — .so плагина: точка входа ll_plugin_* + UI.
#   news_rewriter_tests (опц.)  — юнит-тесты, линкуют только core.
#
# Символы ImGui резолвятся из исполняемого файла приложения (-rdynamic),
# поэтому библиотеки ImGui не линкуются.

cmake_minimum_required(VERSION 3.14)
project(news_rewriter CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

set(NEWS_REWRITER_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PROJECT_ROOT}/include
    ${PROJECT_ROOT}/external/imgui
    ${PROJECT_ROOT}/external/imgui/backends
)

# --- Ядро (чистая логика, без хоста/ImGui) -------------------------------
set(NEWS_REWRITER_CORE_SOURCES
    src/common.cpp
    src/json.cpp
    src/config.cpp
    src/profile.cpp
    src/http.cpp
    src/xml.cpp
    src/fetcher.cpp
    src/extractor.cpp
    src/rewriter.cpp
    src/scheduler.cpp
    src/storage.cpp
    src/dotenv.cpp
    src/sink.cpp
    src/sink_local_file.cpp
    src/sink_http.cpp
    src/sink_wordpress.cpp
    src/worker.cpp
)

add_library(news_rewriter_core STATIC ${NEWS_REWRITER_CORE_SOURCES})
target_include_directories(news_rewriter_core PUBLIC ${NEWS_REWRITER_INCLUDES})
set_target_properties(news_rewriter_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
find_package(Threads REQUIRED)
target_link_libraries(news_rewriter_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
target_compile_options(news_rewriter_core PRIVATE -Wall -Wextra)

# --- Плагин (.so) ----------------------------------------------------------
add_library(news_rewriter SHARED
    src/plugin_main.cpp
    src/ui.cpp
)
target_include_directories(news_rewriter PRIVATE ${NEWS_REWRITER_INCLUDES})
target_link_libraries(news_rewriter PRIVATE news_rewriter_core)
target_compile_options(news_rewriter PRIVATE -Wall -Wextra)

set_target_properties(news_rewriter PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
)

# Манифест копируется рядом с .so под именем <plugin>.json (ищет PluginManager)
add_custom_command(TARGET news_rewriter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
        "${CMAKE_BINARY_DIR}/plugins/news_rewriter.json"
)

# --- Тесты (опционально) ---------------------------------------------------
option(BUILD_NEWS_REWRITER_TESTS "Build news_rewriter unit tests" OFF)
if(BUILD_NEWS_REWRITER_TESTS)
    add_executable(news_rewriter_tests
        tests/test_main.cpp
        tests/test_json.cpp
        tests/test_common.cpp
        tests/test_config.cpp
        tests/test_profile.cpp
        tests/test_worker.cpp
        tests/test_http.cpp
        tests/test_xml.cpp
        tests/test_fetcher.cpp
        tests/test_extractor.cpp
        tests/test_rewriter.cpp
        tests/test_scheduler.cpp
        tests/test_storage.cpp
        tests/test_sink_http.cpp
        tests/test_sink_wordpress.cpp
    )
    target_include_directories(news_rewriter_tests PRIVATE ${NEWS_REWRITER_INCLUDES})
    target_link_libraries(news_rewriter_tests PRIVATE news_rewriter_core)
    target_compile_options(news_rewriter_tests PRIVATE -Wall -Wextra)
endif()

message(STATUS "Building news_rewriter:")
message(STATUS "  Output: ${CMAKE_BINARY_DIR}/plugins/libnews_rewriter.so")
