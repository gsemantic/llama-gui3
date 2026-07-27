# CMakeLists.txt для плагинов системы агентов

cmake_minimum_required(VERSION 3.14)

# Официальные плагины
option(BUILD_OFFICIAL_PLUGINS "Build official plugins" ON)
if(BUILD_OFFICIAL_PLUGINS)
    add_subdirectory(official)
endif()

# Плагины пользователей (если существуют)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/user_plugins/CMakeLists.txt")
    add_subdirectory(user_plugins)
endif()

# Примеры плагинов (для разработки)
option(BUILD_PLUGIN_EXAMPLES "Build plugin examples" OFF)
if(BUILD_PLUGIN_EXAMPLES)
    add_subdirectory(examples)
endif()
