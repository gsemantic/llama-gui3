add_library(managers STATIC)

target_include_directories(managers PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/managers
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/ui
)

# Добавляем все .cpp из текущего каталога
file(GLOB MANAGERS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(managers PRIVATE ${MANAGERS_SOURCES})

# Определить USE_IMGUI для компиляции
target_compile_definitions(managers PRIVATE USE_IMGUI)

# Зависимости от ui (DialogManager и т.д.)
target_link_libraries(managers PRIVATE ui)

# Зависимости от core (для типов из core)
target_link_libraries(managers PRIVATE core)
