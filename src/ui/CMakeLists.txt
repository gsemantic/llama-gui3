add_library(ui STATIC)

target_include_directories(ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/ui
)
# Добавляем все .cpp из текущего каталога
file(GLOB UI_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(ui PRIVATE ${UI_SOURCES})

# Определить USE_IMGUI для компиляции
target_compile_definitions(ui PRIVATE USE_IMGUI)

# Llama Bench интеграция (меню и диалог в UI)
if(ENABLE_LLAMA_BENCH)
    target_compile_definitions(ui PRIVATE ENABLE_LLAMA_BENCH)
endif()

# Определить USE_SDL2 и USE_OPENGL для компиляции
target_compile_definitions(ui PRIVATE USE_SDL2)
target_compile_definitions(ui PRIVATE USE_OPENGL)

# SDL2 зависимости для PerformanceMonitor
if(SDL2_FOUND)
    target_include_directories(ui PUBLIC ${SDL2_INCLUDE_DIRS})
    target_link_libraries(ui PRIVATE ${SDL2_LIBRARIES})
endif()

# Зависимости от core (OpenRouterClient, Settings и т.д.)
target_link_libraries(ui PRIVATE core headless_browser agent_core)
