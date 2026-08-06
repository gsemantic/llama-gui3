cmake_minimum_required(VERSION 3.14)
project(web_search_agent_plugin CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Путь к заголовкам проекта
get_filename_component(PROJECT_ROOT 
    "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

# Создаём разделяемую библиотеку (плагин)
add_library(web_search_agent_plugin SHARED web_search_agent.cpp)

# Включаем заголовки агентов и проекта
target_include_directories(web_search_agent_plugin PRIVATE 
    ${PROJECT_ROOT}/include
    ${PROJECT_ROOT}/include/agents)

# Находим CURL
find_package(CURL REQUIRED)
if(CURL_FOUND)
    target_link_libraries(web_search_agent_plugin PRIVATE ${CURL_LIBRARIES})
    target_include_directories(web_search_agent_plugin PRIVATE ${CURL_INCLUDE_DIRS})
endif()

# Скрываем все символы по умолчанию
set_target_properties(web_search_agent_plugin PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# Вывод информации
message(STATUS "Building web_search_agent plugin:")
message(STATUS "  Output: ${CMAKE_SHARED_LIBRARY_PREFIX}web_search_agent_plugin${CMAKE_SHARED_LIBRARY_SUFFIX}")
message(STATUS "  Install to: plugins/official/web_search_agent/")
