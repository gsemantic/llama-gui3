# CMakeLists.txt для плагинов системы агентов
#
# LEGACY: это старая agent-система (LLM-агенты), перемещённая из plugins/.
# Она отключена и не собирается по умолчанию. Актуальная система плагинов
# находится в plugins/ (см. include/plugins/plugin_api.h и docs/PLUGIN_SYSTEM.md).
# Сборка здесь управляется опцией BUILD_AGENT_PLUGINS (по умолчанию OFF).

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
if(BUILD_PLUGIN_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()
