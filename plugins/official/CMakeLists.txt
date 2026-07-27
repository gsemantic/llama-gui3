# CMakeLists.txt для официальных плагинов системы агентов
# llama-gui official plugins

cmake_minimum_required(VERSION 3.14)

# Rag Agent
option(BUILD_RAG_AGENT "Build RAG agent plugin" ON)
if(BUILD_RAG_AGENT)
    add_subdirectory(rag_agent)
endif()

# File Agent
option(BUILD_FILE_AGENT "Build File agent plugin" ON)
if(BUILD_FILE_AGENT)
    add_subdirectory(file_agent)
endif()

# Web Search Agent
option(BUILD_WEB_SEARCH_AGENT "Build Web Search agent plugin" ON)
if(BUILD_WEB_SEARCH_AGENT)
    add_subdirectory(web_search_agent)
endif()

# Code Agent
option(BUILD_CODE_AGENT "Build Code agent plugin" ON)
if(BUILD_CODE_AGENT)
    add_subdirectory(code_agent)
endif()

# Terminal Agent
option(BUILD_TERMINAL_AGENT "Build Terminal agent plugin" ON)
if(BUILD_TERMINAL_AGENT)
    add_subdirectory(terminal_agent)
endif()

# Summarization Agent
option(BUILD_SUMMARIZATION_AGENT "Build Summarization agent plugin" ON)
if(BUILD_SUMMARIZATION_AGENT)
    add_subdirectory(summarization_agent)
endif()
