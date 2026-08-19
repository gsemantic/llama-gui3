# libs/headless_browser — общая библиотека рендеринга веб-страниц через
# headless-браузер (Chromium). Единый источник правды для основного приложения
# и плагинов (news_rewriter и др.), чтобы не дублировать код запуска браузера.
#
# ВАЖНО: это «headless-браузер» (рендеринг страниц), а НЕ «серверный режим
# (без GUI)» самого приложения llama-gui (флаг --headless в src/main.cpp).

cmake_minimum_required(VERSION 3.14)

add_library(headless_browser STATIC
    src/headless_browser.cpp
)

target_include_directories(headless_browser PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(headless_browser PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_compile_options(headless_browser PRIVATE -Wall -Wextra)
