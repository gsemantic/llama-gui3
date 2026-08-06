cmake_minimum_required(VERSION 3.14)
project(summarization_agent_plugin CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Путь к заголовкам проекта
get_filename_component(PROJECT_ROOT 
    "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

# Создаём разделяемую библиотеку (плагин)
add_library(summarization_agent_plugin SHARED summarization_agent.cpp)

# Включаем заголовки агентов и проекта
target_include_directories(summarization_agent_plugin PRIVATE 
    ${PROJECT_ROOT}/include
    ${PROJECT_ROOT}/include/agents)

# Скрываем все символы по умолчанию
set_target_properties(summarization_agent_plugin PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# Вывод информации
message(STATUS "Building summarization_agent plugin:")
message(STATUS "  Output: ${CMAKE_SHARED_LIBRARY_PREFIX}summarization_agent_plugin${CMAKE_SHARED_LIBRARY_SUFFIX}")
message(STATUS "  Install to: plugins/official/summarization_agent/")
