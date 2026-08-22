cmake_minimum_required(VERSION 3.14)
project("llama-gui-core" C CXX)

# Export compile_commands.json so clangd/LSP resolve include paths correctly
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================================
# Version Information (SemVer 2.0.0)
# ============================================================================
set(VERSION_MAJOR 0)
set(VERSION_MINOR 1)
set(VERSION_PATCH 79)
set(VERSION_SUFFIX "alpha.2")
set(VERSION_CODENAME "Archimedes")

# Full version string: 0.1.0-alpha.2 Archimedes
set(VERSION_STRING "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}-${VERSION_SUFFIX}")
set(VERSION_FULL "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}-${VERSION_SUFFIX} ${VERSION_CODENAME}")

# Build date and time
string(TIMESTAMP BUILD_DATE "%Y-%m-%d")
string(TIMESTAMP BUILD_TIME "%H:%M:%S")
string(TIMESTAMP BUILD_YEAR "%Y")

# Git information (if available)
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
else()
    set(GIT_COMMIT_HASH "unknown")
    set(GIT_DESCRIBE "unknown")
endif()

message(STATUS "=== Llama GUI Core Build Information ===")
message(STATUS "Version: ${VERSION_FULL}")
message(STATUS "Build Date: ${BUILD_DATE} ${BUILD_TIME}")
message(STATUS "Git Commit: ${GIT_COMMIT_HASH}")
message(STATUS "Git Describe: ${GIT_DESCRIBE}")
message(STATUS "========================================")

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable FetchContent for downloading dependencies
include(FetchContent)

# Find nlohmann_json
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# Find CURL
find_package(CURL QUIET)
if(NOT CURL_FOUND)
    FetchContent_Declare(
        CURL
        GIT_REPOSITORY https://github.com/curl/curl.git
        GIT_TAG curl-8_11_0
    )
    FetchContent_MakeAvailable(CURL)
    set(CURL_FOUND TRUE)
endif()

# Find tree-sitter (for AST-aware code parsing)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(TREE_SITTER QUIET tree-sitter)
endif()
if(TREE_SITTER_FOUND)
    message(STATUS "tree-sitter found via pkg-config")
else()
    message(STATUS "tree-sitter not found - AST parsing disabled, using heuristic fallback")
    set(ENABLE_AST_PARSING OFF CACHE BOOL "" FORCE)
endif()

# Tree-sitter language modules (MVP: C, C++, Python, Rust)
if(TREE_SITTER_FOUND)
    set(TS_LANGUAGES c cpp python rust)
    foreach(LANG ${TS_LANGUAGES})
        string(TOUPPER ${LANG} LANG_UPPER)

        # Try to find grammar modules
        if(PkgConfig_FOUND)
            pkg_check_modules(TREE_SITTER_${LANG_UPPER} QUIET tree-sitter-${LANG})
        endif()

        # If not found via pkg-config, use pre-downloaded grammar sources
        if(NOT TREE_SITTER_${LANG_UPPER}_FOUND)
            set(GRAMMAR_SRC_DIR ${CMAKE_SOURCE_DIR}/deps/tree-sitter-${LANG}-src)
            if(EXISTS ${GRAMMAR_SRC_DIR}/src/parser.c)
                # Create static library from grammar sources
                file(GLOB TS_${LANG_UPPER}_SRCS ${GRAMMAR_SRC_DIR}/src/*.c)
                add_library(tree-sitter-grammar-${LANG} STATIC ${TS_${LANG_UPPER}_SRCS})
                set_target_properties(tree-sitter-grammar-${LANG} PROPERTIES LINKER_LANGUAGE C)
                target_include_directories(tree-sitter-grammar-${LANG} PUBLIC
                    ${GRAMMAR_SRC_DIR}/src
                )
                set(TREE_SITTER_${LANG_UPPER}_FOUND TRUE)
                set(TREE_SITTER_${LANG_UPPER}_INCLUDE_DIR ${GRAMMAR_SRC_DIR}/src)
                set(TREE_SITTER_${LANG_UPPER}_LIBRARY tree-sitter-grammar-${LANG})
                message(STATUS "tree-sitter-${LANG} grammar found at ${GRAMMAR_SRC_DIR}")
            else()
                message(WARNING "tree-sitter-${LANG} grammar not found at ${GRAMMAR_SRC_DIR}")
                message(STATUS "Tree-sitter ${LANG} parsing disabled")
            endif()
        else()
            message(STATUS "tree-sitter-${LANG} grammar found via pkg-config")
        endif()
    endforeach()
endif()

# Define AST parsing compile flag
if(TREE_SITTER_FOUND)
    set(ENABLE_AST_PARSING ON CACHE BOOL "" FORCE)
endif()

# Find FAISS (for RAG vector indexing)
find_library(FAISS_LIBRARY faiss PATHS /usr/lib/x86_64-linux-gnu)
find_path(FAISS_INCLUDE_DIR faiss/Index.h PATHS /usr/include)

if(FAISS_LIBRARY AND FAISS_INCLUDE_DIR)
    set(USE_FAISS ON CACHE BOOL "" FORCE)
    message(STATUS "FAISS found: ${FAISS_LIBRARY}")
    message(STATUS "FAISS include: ${FAISS_INCLUDE_DIR}")
else()
    set(USE_FAISS OFF CACHE BOOL "" FORCE)
    message(STATUS "FAISS not found - using brute-force search fallback")
endif()

# Find OpenGL
find_package(OpenGL REQUIRED)

# Find GLEW
find_package(GLEW REQUIRED)

# Find SDL2 (system package)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 QUIET sdl2)
    if(SDL2_FOUND)
        message(STATUS "SDL2 found via pkg-config")
    endif()
endif()

# Fall back to manual detection if pkg-config fails
if(NOT SDL2_FOUND)
    message(WARNING "SDL2 not found via pkg-config, using manual detection")
    set(SDL2_INCLUDE_DIR "/usr/include/SDL2")
    set(SDL2_LIBRARY "/usr/lib/x86_64-linux-gnu/libSDL2.so")
    if(EXISTS "${SDL2_INCLUDE_DIR}" AND EXISTS "${SDL2_LIBRARY}")
        set(SDL2_FOUND TRUE)
        message(STATUS "SDL2 found manually: ${SDL2_LIBRARY}")
        message(STATUS "SDL2 include: ${SDL2_INCLUDE_DIR}")
    else()
        message(WARNING "SDL2 library not found manually")
    endif()
endif()

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/deps/include)
if(SDL2_FOUND)
    include_directories(${SDL2_INCLUDE_DIR})
endif()

# Source files - include all necessary files
set(SOURCES
    src/main.cpp
    src/agents/agent_context.cpp
    src/agents/agent_registry.cpp
    src/agents/plugin_loader.cpp
    src/agents/security_manager.cpp
    src/agents/sandbox.cpp
    src/agents/agent_logger.cpp
    src/agents/agent_config.cpp
    src/bench/profile_adapter.cpp
    src/ui/bench_tab_base.cpp
    src/ui/run_benchmark_tab.cpp
    src/ui/compare_profiles_tab.cpp
    src/ui/results_tab.cpp
    src/ui/history_tab.cpp
)

# Header files
set(HEADERS
    include/core/llama_interface.h
    include/core/llama_interface_impl.h
    include/core/state_manager.h
    include/core/state_manager_conversation.h
    include/core/state_manager_persistence.h
    include/core/settings.h
    include/core/config_manager.h
    include/core/version.h
    include/core/settings_sampling.h
    include/core/settings_model_loading.h
    include/core/settings_gpu.h
    include/core/settings_cache.h
    include/core/settings_rope.h
    include/core/settings_control_vector.h
    include/core/settings_tensor_override.h
    include/core/settings_server_runtime.h
    include/core/settings_batch.h
    include/core/settings_grammar.h
    include/core/settings_output.h
    include/core/model_manager.h
    include/core/chat_template_manager.h
    include/core/rag_manager.h
    include/core/chat_cache_manager.h
    include/core/embedding_generator.h
    include/core/embedding_proxy.h
    include/core/document_parser.h
    include/core/file_dialog_filters.h
    include/core/rag_settings.h
    include/core/openrouter_types.h
    include/core/kv_cache_types.h
    include/core/kv_cache_api.h
    include/core/kv_cache_settings.h
    include/core/kv_cache_storage.h
    include/core/kv_cache_slot_manager.h
    include/core/kv_cache_metrics.h
    include/core/model_performance_stats.h
    include/ui/main_window.h
    include/ui/chat_interface.h
    include/ui/conversation_manager.h
    include/ui/file_manager.h
    include/ui/file_dialog_helper.h
    include/ui/file_browser.h
    include/ui/recent_files_manager.h
    include/ui/settings_dialog.h
    include/ui/settings_dialog_model.h
    include/ui/settings_dialog_gpu.h
    include/ui/settings_dialog_sampling_basic.h
    include/ui/settings_dialog_sampling_advanced.h
    include/ui/settings_dialog_context.h
    include/ui/settings_dialog_rope.h
    include/ui/settings_dialog_advanced.h
    include/ui/settings_dialog_grammar.h
    include/ui/settings_dialog_server_runtime.h
    include/ui/settings_dialog_batch.h
    include/ui/settings_dialog_logging.h
    include/ui/command.h
    include/ui/command_base.h
    include/ui/commands_file.h
    include/ui/commands_help.h
    include/ui/commands_settings.h
    include/ui/commands_workspace.h
    include/ui/command_manager.h
    include/ui/dialog_manager.h
    include/ui/localization_manager.h
    include/ui/language_selector.h
    include/ui/system_prompt_settings.h
    include/ui/rag_interface.h
    include/ui/rag_settings_dialog.h
    include/ui/settings_viewer_dialog.h
    include/ui/workspace_manager.h
    include/ui/quick_settings_dialog.h
    include/ui/advanced_settings_dialog.h
    include/ui/profile_manager_dialog.h
    include/ui/backup_manager_dialog.h
    include/ui/cloud_services_dialog.h
    include/ui/window_manager.h
    include/ui/grid_snapping_system.h
    include/ui/grid_snapping_dialog.h
    include/agents/i_agent.h
    include/agents/agent_types.h
    include/agents/agent_result.h
    include/agents/agent_request.h
    include/agents/agent_capabilities.h
    include/agents/agent_context.h
    include/agents/agent_registry.h
    include/agents/plugin_c_api.h
    include/agents/plugin_loader.h
    include/agents/agents.h
    include/agents/security_manager.h
    include/agents/sandbox.h
    include/agents/agent_logger.h
    include/agents/agent_config.h
    include/bench/bench_types.h
    include/bench/bench_common.h
    include/bench/llama_bench_runner.h
    include/bench/llama_bench_parser.h
    include/bench/llama_bench_results.h
    include/bench/llama_bench_module.h
    include/ui/bench_tab_base.h
    include/ui/run_benchmark_tab.h
    include/ui/compare_profiles_tab.h
    include/ui/results_tab.h
    include/ui/history_tab.h
)

# Add ImGui source files
set(IMGUI_SOURCES
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/imgui_demo.cpp
)

# Add ImGui OpenGL3 backend files (SDL2 backend is optional)
set(IMGUI_OPENGL_SOURCES
    external/imgui/backends/imgui_impl_opengl3.cpp
    external/imgui/backends/imgui_impl_sdl2.cpp
)

# === ImGui as static library (faster builds) ===
add_library(imgui_lib STATIC ${IMGUI_SOURCES} ${IMGUI_OPENGL_SOURCES})
target_include_directories(imgui_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui)
target_include_directories(imgui_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/backends)
if(SDL2_FOUND)
    target_include_directories(imgui_lib PUBLIC /usr/include/SDL2)
endif()
# ==============================================



# ============================================================================
# Agent System - Ядро системы агентов
# ============================================================================
set(AGENT_SOURCES
    src/agents/agent_context.cpp
    src/agents/agent_registry.cpp
    src/agents/plugin_loader.cpp
    src/agents/security_manager.cpp
    src/agents/sandbox.cpp
    src/agents/agent_logger.cpp
    src/agents/agent_config.cpp
)

set(AGENT_HEADERS
    include/agents/i_agent.h
    include/agents/agent_types.h
    include/agents/agent_result.h
    include/agents/agent_request.h
    include/agents/agent_capabilities.h
    include/agents/agent_context.h
    include/agents/agent_registry.h
    include/agents/plugin_c_api.h
    include/agents/plugin_loader.h
    include/agents/agents.h
    include/agents/security_manager.h
    include/agents/sandbox.h
    include/agents/agent_logger.h
    include/agents/agent_config.h
)

# Создаём статическую библиотеку ядра агентов
add_library(agent_core STATIC ${AGENT_SOURCES} ${AGENT_HEADERS})
target_include_directories(agent_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(agent_core PRIVATE nlohmann_json::nlohmann_json headless_browser)

# Добавляем dl для динамической загрузки на Linux
if(UNIX AND NOT APPLE)
    target_link_libraries(agent_core PRIVATE ${CMAKE_DL_LIBS})
endif()

# ============================================================================
# Create the main executable with all sources
# Add subdirectories for modular libraries

# Общая библиотека headless-браузера (Chromium). Объявляется ДО модулей и
# плагинов, чтобы цель headless_browser была видна везде (основное приложение,
# agent_core и плагин news_rewriter линкуются к ней).
add_subdirectory(libs/headless_browser)

add_subdirectory(src/core)
add_subdirectory(src/ui)
add_subdirectory(src/managers)
add_subdirectory(src/plugins)

# Build the main executable linking the new libraries (only main.cpp is needed here)
add_executable(llama-gui-core src/main.cpp)
# Link all modular static libraries
target_link_libraries(llama-gui-core PRIVATE
    core
    ui
    managers
    plugin_core
    agent_core
    headless_browser
    imgui_lib
    CURL::libcurl
)

# Экспорт символов в динамические библиотеки (для плагинов, использующих ImGui)
# Позволяет .so плагинам резолвить символы ImGui из исполняемого файла.
if(UNIX AND NOT APPLE)
    target_link_options(llama-gui-core PRIVATE "-rdynamic")
endif()

# Link SDL2
if(SDL2_FOUND)
    target_link_libraries(llama-gui-core PRIVATE ${SDL2_LIBRARY})
endif()
target_compile_definitions(llama-gui-core PRIVATE USE_SDL2)

# Link OpenGL and GLEW
target_link_libraries(llama-gui-core PRIVATE OpenGL::GL GLEW::GLEW)
target_compile_definitions(llama-gui-core PRIVATE USE_OPENGL)

# ============================================================================
# Version Definitions - Передача версии в код
# ============================================================================

# Build type and compiler information
set(BUILD_TYPE "${CMAKE_BUILD_TYPE}")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(COMPILER_VERSION "GCC ${CMAKE_CXX_COMPILER_VERSION}")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(COMPILER_VERSION "Clang ${CMAKE_CXX_COMPILER_VERSION}")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(COMPILER_VERSION "MSVC ${CMAKE_CXX_COMPILER_VERSION}")
else()
    set(COMPILER_VERSION "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endif()

target_compile_definitions(llama-gui-core PRIVATE
    LLAMA_GUI_VERSION_MAJOR=${VERSION_MAJOR}
    LLAMA_GUI_VERSION_MINOR=${VERSION_MINOR}
    LLAMA_GUI_VERSION_PATCH=${VERSION_PATCH}
    LLAMA_GUI_VERSION_SUFFIX="${VERSION_SUFFIX}"
    LLAMA_GUI_VERSION_CODENAME="${VERSION_CODENAME}"
    LLAMA_GUI_VERSION_STRING="${VERSION_STRING}"
    LLAMA_GUI_VERSION_FULL="${VERSION_FULL}"
    BUILD_DATE="${BUILD_DATE}"
    BUILD_TIME="${BUILD_TIME}"
    BUILD_YEAR="${BUILD_YEAR}"
    GIT_COMMIT_HASH="${GIT_COMMIT_HASH}"
    GIT_DESCRIBE="${GIT_DESCRIBE}"
    BUILD_TYPE="${BUILD_TYPE}"
    COMPILER_VERSION="${COMPILER_VERSION}"
)

# ============================================================================
# Llama Bench Integration - Опциональная сборка
# ============================================================================
option(ENABLE_LLAMA_BENCH "Enable Llama Bench integration" ON)
if(ENABLE_LLAMA_BENCH)
    add_definitions(-DENABLE_LLAMA_BENCH)

    set(BENCH_SOURCES
        src/bench/llama_bench_runner.cpp
        src/bench/llama_bench_parser.cpp
        src/bench/llama_bench_results.cpp
        src/bench/llama_bench_module.cpp
    )

    set(BENCH_HEADERS
        include/bench/bench_types.h
        include/bench/bench_common.h
        include/bench/llama_bench_runner.h
        include/bench/llama_bench_parser.h
        include/bench/llama_bench_results.h
        include/bench/llama_bench_module.h
    )

    # Добавить исходники bench в основной executable
    target_sources(llama-gui-core PRIVATE ${BENCH_SOURCES} ${BENCH_HEADERS})

    # Копирование директории результатов в build
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/bench_results)
        file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/bench_results/
             DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/bench_results/)
        message(STATUS "Bench results directory copied to build directory")
    endif()

    message(STATUS "Llama Bench integration enabled")
else()
    message(STATUS "Llama Bench integration disabled")
endif()

# ============================================================================
# GUI Integration - ImGui
# ============================================================================
set(USE_IMGUI ON)
target_compile_definitions(llama-gui-core PRIVATE USE_IMGUI)

# ============================================================================
# Plugins - Сборка плагинов приложения
# (примеры в plugins/examples, пользовательские в plugins/user_plugins)
option(BUILD_PLUGINS "Build application plugins" ON)
if(BUILD_PLUGINS)
    add_subdirectory(plugins)
endif()

# ============================================================================
# Tests
# ============================================================================
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
