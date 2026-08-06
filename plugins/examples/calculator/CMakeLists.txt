# calculator — пример плагина: калькулятор выражений
#
# Автономный CMakeLists: подключает только SDK и Dear ImGui, выводит .so
# в build/plugins/. Символы ImGui резолвятся из исполняемого файла
# приложения (-rdynamic), поэтому библиотеки не линкуются.

cmake_minimum_required(VERSION 3.14)
project(calculator CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

add_library(calculator SHARED calculator.cpp)

target_include_directories(calculator PRIVATE
    ${PROJECT_ROOT}/include
    ${PROJECT_ROOT}/external/imgui
    ${PROJECT_ROOT}/external/imgui/backends
)

# Собираем в build/plugins/, где приложение ищет плагины по умолчанию
set_target_properties(calculator PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
)

# Манифест plugin.json копируется рядом с .so
add_custom_command(TARGET calculator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
        "${CMAKE_BINARY_DIR}/plugins/calculator.json"
)

message(STATUS "Building calculator:")
message(STATUS "  Output: ${CMAKE_BINARY_DIR}/plugins/libcalculator.so")
