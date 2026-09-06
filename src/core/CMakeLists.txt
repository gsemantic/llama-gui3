add_library(core STATIC)

target_include_directories(core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/core
)

# Link CURL
if(CURL_FOUND)
    target_link_libraries(core PRIVATE CURL::libcurl)
    target_compile_definitions(core PRIVATE USE_CURL)
endif()

# Link FAISS (for RAG vector indexing)
if(USE_FAISS)
    find_package(OpenMP)
    find_package(BLAS)
    if(OpenMP_CXX_FOUND AND BLAS_FOUND)
        target_link_libraries(core PRIVATE ${FAISS_LIBRARY} OpenMP::OpenMP_CXX ${BLAS_LIBRARIES})
    elseif(OpenMP_CXX_FOUND)
        target_link_libraries(core PRIVATE ${FAISS_LIBRARY} OpenMP::OpenMP_CXX openblas)
    else()
        target_link_libraries(core PRIVATE ${FAISS_LIBRARY} gomp openblas)
    endif()
    target_include_directories(core PRIVATE ${FAISS_INCLUDE_DIR})
    target_compile_definitions(core PRIVATE USE_FAISS)
    message(STATUS "FAISS enabled for RAG indexing")
endif()

# Link tree-sitter language modules (BEFORE tree-sitter for link order)
foreach(LANG c cpp python rust)
    string(TOUPPER ${LANG} LANG_UPPER)
    if(TREE_SITTER_${LANG_UPPER}_FOUND)
        target_link_libraries(core PUBLIC ${TREE_SITTER_${LANG_UPPER}_LIBRARY})
        target_include_directories(core PUBLIC ${TREE_SITTER_${LANG_UPPER}_INCLUDE_DIR})
    endif()
endforeach()

# Link tree-sitter (after grammar libraries for correct link order)
if(TREE_SITTER_FOUND)
    target_link_libraries(core PUBLIC ${TREE_SITTER_LIBRARIES})
    target_include_directories(core PUBLIC ${TREE_SITTER_INCLUDE_DIRS})
    target_compile_definitions(core PUBLIC ENABLE_AST_PARSING)
endif()

# Явный список всех исходников (включая rag_manager*.cpp)
set(CORE_SOURCES
    llama_interface.cpp
    llama_interface_impl.cpp
    cloud_proxy.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/bench/profile_adapter.cpp
    server_manager.cpp
    model_manager.cpp
    rag_manager_core.cpp
    rag_manager_cache.cpp
    rag_manager_search.cpp
    rag_manager_hybrid.cpp
    rag_manager_documents.cpp
    rag_manager_embedding.cpp
    rag_manager_persistence.cpp
    rag_manager_progress.cpp
    rag_manager_proxy.cpp
    rag_manager_deep_analysis.cpp
    rag_manager_deep_analysis_mapreduce.cpp
    rag_manager_deep_analysis_modes.cpp
    rag_manager_deep_analysis_synthesis.cpp
    rag_manager_utils.cpp
    rag_index_profile.cpp
    stemmer.cpp
    chat_cache_manager.cpp
    chat_template_manager.cpp
    config_manager.cpp
    document_parser.cpp
    embedding_generator.cpp
    embedding_proxy.cpp
    embedding_server.cpp
    env_manager.cpp
    tor_bridge_manager.cpp
    file_dialog_filters.cpp
    ini_section_parser.cpp
    kv_cache_api_impl.cpp
    kv_cache_metrics.cpp
    kv_cache_settings.cpp
    kv_cache_slot_manager.cpp
    kv_cache_storage.cpp
    model_performance_stats.cpp
    openrouter_client.cpp
    openrouter_http_client.cpp
    openrouter_model_parser.cpp
    openrouter_rate_limiter.cpp
    settings_accessors.cpp
    settings_core.cpp
    settings_ini.cpp
    settings_model.cpp
    profile_manager.cpp
    settings_profiles.cpp
    settings_snapshot.cpp
    settings_serialization_advanced.cpp
    settings_serialization_basic.cpp
    settings_serialization_server.cpp
    settings_serialization_openrouter.cpp
    settings_serialization.cpp
    settings_validation.cpp
    settings_window.cpp
    state_manager_conversation.cpp
    state_manager_persistence.cpp
    state_manager.cpp
    workspace_settings_integration.cpp
    context_monitor.cpp
    context_compressor.cpp
    ast_parser.cpp
    ast_parser_languages.cpp
    streaming_generator.cpp
)

target_sources(core PRIVATE ${CORE_SOURCES})
