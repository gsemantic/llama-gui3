# Plugin system host — управление загрузкой/выгрузкой плагинов
add_library(plugin_core STATIC
    plugin_manager.cpp
)

target_include_directories(plugin_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

target_compile_definitions(plugin_core PRIVATE USE_IMGUI)

# Зависимости от подсистем приложения
target_link_libraries(plugin_core PRIVATE
    core
    ui
)

# dl для динамической загрузки на Linux
if(UNIX AND NOT APPLE)
    target_link_libraries(plugin_core PRIVATE ${CMAKE_DL_LIBS})
endif()
