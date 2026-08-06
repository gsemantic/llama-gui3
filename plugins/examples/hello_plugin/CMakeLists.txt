# hello_plugin — пример плагина приложения
#
# Собирается как разделяемая библиотека (.so), которую PluginManager
# приложения загружает из build/plugins/. Плагин подключает только SDK:
#   - include/plugins/plugin_api.h   — C-API хоста
#   - external/imgui/imgui.h          — Dear ImGui для отрисовки своих окон
#
# Символы ImGui резолвятся из исполняемого файла приложения (см. -rdynamic
# в корневом CMakeLists.txt), поэтому никакие библиотеки не линкуются.

cmake_minimum_required(VERSION 3.14)
project(hello_plugin CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

add_library(hello_plugin SHARED hello_plugin.cpp)

target_include_directories(hello_plugin PRIVATE
    ${PROJECT_ROOT}/include
    ${PROJECT_ROOT}/external/imgui
    ${PROJECT_ROOT}/external/imgui/backends
)

# Собираем в build/plugins/, где приложение ищет плагины по умолчанию
set_target_properties(hello_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
)

# Манифест plugin.json копируется рядом с .so (PluginManager читает его при загрузке)
add_custom_command(TARGET hello_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
        "${CMAKE_BINARY_DIR}/plugins/plugin.json"
)

message(STATUS "Building hello_plugin:")
message(STATUS "  Output: ${CMAKE_BINARY_DIR}/plugins/libhello_plugin.so")
