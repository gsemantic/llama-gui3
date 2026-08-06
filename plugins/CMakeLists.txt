# CMakeLists.txt для плагинов приложения llama-gui
#
# Эта директория принадлежит общей системе плагинов (include/plugins/plugin_api.h).
# Старая agent-система перемещена в plugins/agents/ и собирается только
# если явно включена (BUILD_AGENT_PLUGINS=ON).

cmake_minimum_required(VERSION 3.14)

# Примеры плагинов (демонстрируют API: меню, окна, команды, диалоги, RAG)
option(BUILD_PLUGIN_EXAMPLES "Build plugin examples" ON)
if(BUILD_PLUGIN_EXAMPLES)
    add_subdirectory(examples)
endif()

# Пользовательские плагины (разрабатываются независимо)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/user_plugins/CMakeLists.txt")
    add_subdirectory(user_plugins)
endif()

# Старая agent-система плагинов (LLM-агенты) — отключена по умолчанию.
option(BUILD_AGENT_PLUGINS "Build legacy agent plugins" OFF)
if(BUILD_AGENT_PLUGINS)
    add_subdirectory(agents)
endif()
