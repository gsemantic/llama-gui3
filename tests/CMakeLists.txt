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

# Model catalog tests: проекция models.dev, сопоставление по endpoint.
# Сеть не нужна — проверяются чистые функции разбора и матчинга.
add_executable(test_model_catalog core/test_model_catalog.cpp)
target_link_libraries(test_model_catalog PRIVATE core nlohmann_json::nlohmann_json)
target_include_directories(test_model_catalog PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME ModelCatalog COMMAND test_model_catalog)

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

# RAG registry: скоупы, пути, квоты (R1.2). Без FAISS и без эмбеддингов:
# проверяются чистые функции и реестр (ленивое создание, изоляция,
# LRU-вытеснение). Тест обязан падать на `../` в имени плагина.
add_executable(test_rag_registry core/test_rag_registry.cpp)
target_link_libraries(test_rag_registry PRIVATE core)
target_include_directories(test_rag_registry PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME RagRegistry COMMAND test_rag_registry)

# RagStore: один изолированный индекс (R1.1).
# Проверяет контракт стора без эмбеддингов: каталог, честный info(),
# index_document=0 без эмбеддингов, clear() не трогает соседний стор.
add_executable(test_rag_store core/test_rag_store.cpp)
target_link_libraries(test_rag_store PRIVATE core)
target_include_directories(test_rag_store PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME RagStore COMMAND test_rag_store)

# Квоты индексации (R4.2): max_bytes/max_chunks/max_index_bytes/дедлайн,
# частичный результат (truncated) и reindex. Эмбеддинги — n-gram фолбэк.
add_executable(test_rag_quota core/test_rag_quota.cpp)
target_link_libraries(test_rag_quota PRIVATE core)
target_include_directories(test_rag_quota PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME RagQuota COMMAND test_rag_quota)

# Изоляция скоупов (R1.6): документ A не находится в B, clear(A) не трогает
# B, повторная индексация не дублирует чанки. Эмбеддинги — детерминированный
# n-gram фолбэк (недостижимый server_url), без модели и без сети.
add_executable(test_rag_scope_isolation core/test_rag_scope_isolation.cpp)
target_link_libraries(test_rag_scope_isolation PRIVATE core)
target_include_directories(test_rag_scope_isolation PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME RagScopeIsolation COMMAND test_rag_scope_isolation)

# ABI RAG v2 (R2.2): скоупы и JSON-контракт host_rag_*_ex. Тест зовёт
# реализацию напрямую (plugin_rag_api.cpp), без PluginManager и без
# LlamaPluginHost: слой ABI должен быть проверяемым.
add_executable(test_plugin_rag_api core/test_plugin_rag_api.cpp)
target_link_libraries(test_plugin_rag_api PRIVATE plugin_core core)
target_include_directories(test_plugin_rag_api PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/plugins)
add_test(NAME PluginRagApi COMMAND test_plugin_rag_api)

# Права RAG из манифеста (R3.1): `_ex` отказывают без rag_access/rag_write,
# легаси пока предупреждает. Проверяется через plugin_rag_api.cpp напрямую,
# без PluginManager: plugin_has_permission — чистая функция.
add_executable(test_plugin_rag_permissions core/test_plugin_rag_permissions.cpp)
target_link_libraries(test_plugin_rag_permissions PRIVATE plugin_core core)
target_include_directories(test_plugin_rag_permissions PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/plugins)
add_test(NAME PluginRagPermissions COMMAND test_plugin_rag_permissions)

# Схема манифеста (R2.5): rag_write в enum прав, блок rag, additionalProperties.
# Тест читает схему и манифесты из ДЕРЕВА ИСХОДНИКОВ, а не из build/.
add_executable(test_plugin_manifest_schema core/test_plugin_manifest_schema.cpp)
target_link_libraries(test_plugin_manifest_schema PRIVATE core)
target_include_directories(test_plugin_manifest_schema PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_plugin_manifest_schema PRIVATE
    PLUGIN_SCHEMA_PATH="${CMAKE_SOURCE_DIR}/plugins/plugin.schema.json"
    PLUGIN_MANIFEST_DIR="${CMAKE_SOURCE_DIR}/plugins")
add_test(NAME PluginManifestSchema COMMAND test_plugin_manifest_schema)

# RAG embedding unavailability contract (R0.1).
# Проверяет, что при отсутствии модели/сервера generate_embedding
# возвращает ПУСТОЙ вектор, индекс чат-истории не растёт, а
# build_rag_prompt не падает на пустых chunks (раньше там было деление
# на ноль). Тест не требует FAISS: он проверяет именно контракт
# «пусто вместо мусора».
add_executable(test_rag_embedding_unavailable core/test_rag_embedding_unavailable.cpp)
target_link_libraries(test_rag_embedding_unavailable PRIVATE core)
target_include_directories(test_rag_embedding_unavailable PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME RagEmbeddingUnavailable COMMAND test_rag_embedding_unavailable)

# RAG gate for plugins (R0.2).
# Проверяет единственное место решения «отвечать ли на rag_*»:
# settings.rag().enable_rag должен гасить RAG и для плагинов, а не только
# в собственном UI чата. Тест не грузит плагины: plugin_rag_allowed
# читает только указатели subsystems.
add_executable(test_plugin_rag_gate core/test_plugin_rag_gate.cpp)
target_link_libraries(test_plugin_rag_gate PRIVATE plugin_core core)
target_include_directories(test_plugin_rag_gate PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/plugins)
add_test(NAME PluginRagGate COMMAND test_plugin_rag_gate)

# Plugin identity: имя плагина как идентификатор (R0.3).
# Проверяет plugin_name_is_valid / plugin_name_sanitize на десятках имён:
# валидные, враждебные (`../etc/passwd`), не-ASCII, границы длины,
# идемпотентность. Тест не грузит плагины и не читает манифесты.
add_executable(test_plugin_identity core/test_plugin_identity.cpp)
target_link_libraries(test_plugin_identity PRIVATE plugin_core core)
target_include_directories(test_plugin_identity PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/plugins)
add_test(NAME PluginIdentity COMMAND test_plugin_identity)

# Document Parser tests (XML/Mallard/DocBook/HTML text extraction)
add_executable(test_document_parser core/test_document_parser.cpp)
target_link_libraries(test_document_parser PRIVATE core)
target_include_directories(test_document_parser PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME DocumentParser COMMAND test_document_parser)

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

# Menu Layout Manager tests (модель раскладки и JSON, без рендера)
add_executable(test_menu_layout core/test_menu_layout.cpp)
target_sources(test_menu_layout PRIVATE ${CMAKE_SOURCE_DIR}/src/ui/menu_layout_manager.cpp)
target_link_libraries(test_menu_layout PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(test_menu_layout PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME MenuLayout COMMAND test_menu_layout)

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

# Chat request tests: тело POST /v1/chat/completions и вывод о поддержке
# tools (И6.2). Обе функции чистые — ни сети, ни состояния.
add_executable(test_chat_request core/test_chat_request.cpp)
target_link_libraries(test_chat_request PRIVATE core nlohmann_json::nlohmann_json)
target_include_directories(test_chat_request PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME ChatRequest COMMAND test_chat_request)

# Markdown renderer: generic code block (lang unknown => keywords == nullptr)
# не должен разыменовывать null в is_keyword.
add_executable(test_markdown_renderer core/test_markdown_renderer.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/markdown_renderer.cpp)
target_link_libraries(test_markdown_renderer PRIVATE imgui_lib)
target_compile_definitions(test_markdown_renderer PRIVATE USE_IMGUI)
target_include_directories(test_markdown_renderer PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME MarkdownRenderer COMMAND test_markdown_renderer)

# AST symbols tests: символы файла для плагина (И6.3). Словарь видов и формат
# проверяются без грамматик; сквозная проверка — только при TREE_SITTER_FOUND.
add_executable(test_ast_symbols core/test_ast_symbols.cpp)
target_link_libraries(test_ast_symbols PRIVATE plugin_core core nlohmann_json::nlohmann_json)
target_include_directories(test_ast_symbols PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/plugins)
# Link grammar libraries BEFORE tree-sitter (linker resolves left-to-right)
if(TREE_SITTER_FOUND)
    foreach(LANG c cpp python rust)
        string(TOUPPER ${LANG} LANG_UPPER)
        if(TREE_SITTER_${LANG_UPPER}_FOUND)
            target_link_libraries(test_ast_symbols PRIVATE ${TREE_SITTER_${LANG_UPPER}_LIBRARY})
        endif()
    endforeach()
    target_link_libraries(test_ast_symbols PRIVATE tree-sitter)
    target_compile_definitions(test_ast_symbols PRIVATE TREE_SITTER_FOUND=1)
endif()
add_test(NAME AstSymbols COMMAND test_ast_symbols)

# LLM stream bridge tests: раскладка чанков провайдера на колбэки плагина.
# Мост чистый (сети нет), поэтому проверяется без PluginManager и сервера.
add_executable(test_llm_stream_bridge core/test_llm_stream_bridge.cpp
    ${CMAKE_SOURCE_DIR}/src/plugins/llm_stream_bridge.cpp)
target_link_libraries(test_llm_stream_bridge PRIVATE plugin_core nlohmann_json::nlohmann_json)
target_include_directories(test_llm_stream_bridge PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/plugins)
add_test(NAME LlmStreamBridge COMMAND test_llm_stream_bridge)

# Plugin loader tests (loads build/plugins/libhello_plugin.so via PluginManager)
if(BUILD_PLUGINS)
    # Тестовый бинарник — «мини-хост» для плагинов: часть плагинов (wp_coder)
    # не линкует ImGui, а рассчитывает на символы хоста. Хост отдаёт их через
    # -rdynamic, поэтому тест должен собрать тот же контракт: ImGui целиком
    # (в тесте нет ни одного статического вызова ImGui, и без whole-archive
    # линкер не тянет imgui.cpp из архива) + экспорт символов в .dynsym.
    # SDL2/OpenGL нужны бэкендам (imgui_impl_sdl2/opengl3), попадающим в архив.
    add_executable(test_plugin_loader core/test_plugin_loader.cpp)
    target_link_libraries(test_plugin_loader PRIVATE
        plugin_core ui core managers
        "-Wl,--whole-archive" imgui_lib "-Wl,--no-whole-archive")
    target_link_options(test_plugin_loader PRIVATE "-rdynamic")
    target_include_directories(test_plugin_loader PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_compile_definitions(test_plugin_loader PRIVATE PLUGIN_TEST_DIR="${CMAKE_BINARY_DIR}/plugins")
    if(SDL2_FOUND)
        target_link_libraries(test_plugin_loader PRIVATE ${SDL2_LIBRARIES})
    endif()
    target_link_libraries(test_plugin_loader PRIVATE GLEW::GLEW OpenGL::GL)
    if(UNIX AND NOT APPLE)
        target_link_libraries(test_plugin_loader PRIVATE ${CMAKE_DL_LIBS})
    endif()
    add_test(NAME PluginLoader COMMAND test_plugin_loader)

    # Разбор messages_json — граница ABI между хостом и плагином (мульти-
    # модальность). Проверяется на plugin_core: parse_messages_json живёт в
    # plugin_manager.cpp, и без этой библиотеки тест линковал бы заглушку.
    # Те же зависимости, что у PluginLoader: whole-archive imgui нужен
    # линкеру, иначе символы виджетов не тянутся из архива.
    add_executable(test_plugin_messages_json core/test_plugin_messages_json.cpp)
    target_link_libraries(test_plugin_messages_json PRIVATE
        plugin_core ui core managers
        "-Wl,--whole-archive" imgui_lib "-Wl,--no-whole-archive")
    target_link_options(test_plugin_messages_json PRIVATE "-rdynamic")
    target_include_directories(test_plugin_messages_json PRIVATE ${CMAKE_SOURCE_DIR}/include)
    if(SDL2_FOUND)
        target_link_libraries(test_plugin_messages_json PRIVATE ${SDL2_LIBRARIES})
    endif()
    target_link_libraries(test_plugin_messages_json PRIVATE GLEW::GLEW OpenGL::GL)
    if(UNIX AND NOT APPLE)
        target_link_libraries(test_plugin_messages_json PRIVATE ${CMAKE_DL_LIBS})
    endif()
    add_test(NAME PluginMessagesJson COMMAND test_plugin_messages_json)

    # Восстановление пунктов меню плагинов после перестройки меню приложением
    add_executable(test_menu_restore core/test_menu_restore.cpp)
    target_link_libraries(test_menu_restore PRIVATE
        plugin_core ui core managers
        "-Wl,--whole-archive" imgui_lib "-Wl,--no-whole-archive")
    target_link_options(test_menu_restore PRIVATE "-rdynamic")
    target_include_directories(test_menu_restore PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_compile_definitions(test_menu_restore PRIVATE PLUGIN_TEST_DIR="${CMAKE_BINARY_DIR}/plugins")
    if(SDL2_FOUND)
        target_link_libraries(test_menu_restore PRIVATE ${SDL2_LIBRARIES})
    endif()
    target_link_libraries(test_menu_restore PRIVATE GLEW::GLEW OpenGL::GL)
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
    DEPENDS test_openrouter_model_parser test_model_catalog test_rag_deep_analysis_utils test_rag_chunking test_document_parser test_dialog_manager test_context_monitor test_ast_parser test_menu_layout test_llm_stream_bridge test_markdown_renderer test_plugin_loader test_plugin_messages_json test_menu_restore test_ui_auditor
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
