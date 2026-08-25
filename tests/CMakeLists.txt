enable_testing()

# Test framework
add_library(test_framework INTERFACE)
target_include_directories(test_framework INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# ============================================================================
# Core tests (no SDL2 dependency)
# ============================================================================

# OpenRouter Model Parser tests
add_executable(test_openrouter_model_parser core/test_openrouter_model_parser.cpp)
target_link_libraries(test_openrouter_model_parser PRIVATE core nlohmann_json::nlohmann_json)
target_include_directories(test_openrouter_model_parser PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME OpenRouterModelParser COMMAND test_openrouter_model_parser)

# RAG Deep Analysis Utils tests
add_executable(test_rag_deep_analysis_utils core/test_rag_deep_analysis_utils.cpp)
target_link_libraries(test_rag_deep_analysis_utils PRIVATE core)
target_include_directories(test_rag_deep_analysis_utils PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME RagDeepAnalysisUtils COMMAND test_rag_deep_analysis_utils)

# RAG Chunking and Stemmer tests
add_executable(test_rag_chunking core/test_rag_chunking.cpp)
target_link_libraries(test_rag_chunking PRIVATE core)
target_include_directories(test_rag_chunking PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME RagChunking COMMAND test_rag_chunking)

# Context Monitor tests
add_executable(test_context_monitor test_context_monitor.cpp)
target_link_libraries(test_context_monitor PRIVATE core)
target_include_directories(test_context_monitor PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME ContextMonitor COMMAND test_context_monitor)

# AST Parser tests
add_executable(test_ast_parser test_ast_parser.cpp)
target_include_directories(test_ast_parser PRIVATE ${CMAKE_SOURCE_DIR}/include)
# Link grammar libraries BEFORE tree-sitter (linker resolves left-to-right)
if(TREE_SITTER_FOUND)
    foreach(LANG c cpp python rust)
        string(TOUPPER ${LANG} LANG_UPPER)
        if(TREE_SITTER_${LANG_UPPER}_FOUND)
            target_link_libraries(test_ast_parser PRIVATE ${TREE_SITTER_${LANG_UPPER}_LIBRARY})
        endif()
    endforeach()
    target_link_libraries(test_ast_parser PRIVATE core tree-sitter)
endif()
add_test(NAME AstParser COMMAND test_ast_parser)

# Dialog Manager tests (struct-level only, no rendering)
add_executable(test_dialog_manager core/test_dialog_manager.cpp)
target_link_libraries(test_dialog_manager PRIVATE managers)
target_include_directories(test_dialog_manager PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME DialogManager COMMAND test_dialog_manager)

# UI Auditor tests: регрессионная защита консистентности меню/команд
# (битые ссылки, заглушки, дубли, сироты). Без рендера — только реестры.
add_executable(test_ui_auditor core/test_ui_auditor.cpp)
# Внутри libui циклические зависимости членов (advanced_menu_system ↔
# window_manager ↔ grid_snapping ↔ main_window); группа позволяет ld
# пересканировать архивы до полного разрешения без дублей членов.
target_link_libraries(test_ui_auditor PRIVATE
    "-Wl,--start-group"
    ui core managers imgui_lib plugin_core
    "-Wl,--end-group")
target_compile_definitions(test_ui_auditor PRIVATE USE_IMGUI USE_SDL2 USE_OPENGL)
target_include_directories(test_ui_auditor PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(SDL2_FOUND)
    target_link_libraries(test_ui_auditor PRIVATE ${SDL2_LIBRARIES})
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(test_ui_auditor PRIVATE ${CMAKE_DL_LIBS})
endif()
# Члены ui ссылаются на OpenGL и бенчмарки
target_link_libraries(test_ui_auditor PRIVATE GLEW::GLEW OpenGL::GL CURL::libcurl)
if(ENABLE_LLAMA_BENCH)
    target_sources(test_ui_auditor PRIVATE
        ${CMAKE_SOURCE_DIR}/src/bench/llama_bench_runner.cpp
        ${CMAKE_SOURCE_DIR}/src/bench/llama_bench_parser.cpp
        ${CMAKE_SOURCE_DIR}/src/bench/llama_bench_results.cpp
        ${CMAKE_SOURCE_DIR}/src/bench/llama_bench_module.cpp)
endif()
add_test(NAME UiAuditor COMMAND test_ui_auditor)

# Plugin loader tests (loads build/plugins/libhello_plugin.so via PluginManager)
if(BUILD_PLUGINS)
    add_executable(test_plugin_loader core/test_plugin_loader.cpp)
    target_link_libraries(test_plugin_loader PRIVATE plugin_core ui core managers imgui_lib)
    target_include_directories(test_plugin_loader PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_compile_definitions(test_plugin_loader PRIVATE PLUGIN_TEST_DIR="${CMAKE_BINARY_DIR}/plugins")
    if(UNIX AND NOT APPLE)
        target_link_libraries(test_plugin_loader PRIVATE ${CMAKE_DL_LIBS})
    endif()
    add_test(NAME PluginLoader COMMAND test_plugin_loader)

    # Восстановление пунктов меню плагинов после перестройки меню приложением
    add_executable(test_menu_restore core/test_menu_restore.cpp)
    target_link_libraries(test_menu_restore PRIVATE plugin_core ui core managers imgui_lib)
    target_include_directories(test_menu_restore PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_compile_definitions(test_menu_restore PRIVATE PLUGIN_TEST_DIR="${CMAKE_BINARY_DIR}/plugins")
    if(UNIX AND NOT APPLE)
        target_link_libraries(test_menu_restore PRIVATE ${CMAKE_DL_LIBS})
    endif()
    add_test(NAME MenuRestore COMMAND test_menu_restore)
endif()

# ============================================================================
# Test target
# ============================================================================

add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -j4
    DEPENDS test_openrouter_model_parser test_rag_deep_analysis_utils test_rag_chunking test_dialog_manager test_context_monitor test_ast_parser test_plugin_loader test_menu_restore test_ui_auditor
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
