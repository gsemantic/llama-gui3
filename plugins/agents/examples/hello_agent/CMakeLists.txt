cmake_minimum_required(VERSION 3.14)
project(hello_agent_plugin CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Путь к заголовкам агентов
get_filename_component(AGENT_INCLUDE_DIR 
    "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

# Создаём разделяемую библиотеку (плагин)
add_library(hello_agent_plugin SHARED hello_agent.cpp)

# Включаем заголовки агентов
target_include_directories(hello_agent_plugin PRIVATE 
    ${AGENT_INCLUDE_DIR}/include)

# Скрываем все символы по умолчанию (экспортируем только нужные)
set_target_properties(hello_agent_plugin PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# Выводим имя файла плагина
message(STATUS "Building hello_agent plugin:")
message(STATUS "  Output: ${CMAKE_SHARED_LIBRARY_PREFIX}hello_agent_plugin${CMAKE_SHARED_LIBRARY_SUFFIX}")
message(STATUS "  Install to: plugins/")
