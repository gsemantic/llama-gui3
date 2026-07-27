/**
 * @file rag_agent.cpp
 * @brief Реализация RAG-агента для поиска по документам
 */

#include "rag_agent.h"
#include <iostream>
#include <sstream>

// Include RagManager for real integration
#include <core/rag_manager.h>

namespace agents {

// ============================================================================
// RagAgent implementation
// ============================================================================

RagAgent::RagAgent() = default;

RagAgent::~RagAgent() {
    shutdown();
}

const char* RagAgent::name() const {
    return "rag_agent";
}

const char* RagAgent::description() const {
    return "RAG-based document search and retrieval agent. "
           "Provides semantic search over loaded documents using embeddings.";
}

const char* RagAgent::version() const {
    return "1.0.0";
}

bool RagAgent::initialize(AgentContext* context) {
    if (!context) {
        return false;
    }
    
    context_ = context;
    initialized_ = true;
    
    // Загрузка настроек из конфигурации
    if (context_) {
        max_chunks_ = context_->get_agent_config(name()).value("max_chunks", 10);
        similarity_threshold_ = context_->get_agent_config(name()).value(
            "similarity_threshold", 0.7f);
        cache_enabled_ = context_->get_agent_config(name()).value(
            "cache_enabled", true);
    }
    
    context_->info(name(), "Initialized with max_chunks=" + 
                   std::to_string(max_chunks_));
    
    return true;
}

AgentResult RagAgent::execute(const AgentRequest& request) {
    if (!initialized_) {
        return AgentResult::error("Agent not initialized");
    }
    
    std::string action = request.action();
    
    if (context_) {
        context_->debug(name(), "Executing action: " + action);
    }
    
    if (action == "search") {
        return handle_search(request);
    } else if (action == "add_document") {
        return handle_add_document(request);
    } else if (action == "list_documents") {
        return handle_list_documents(request);
    } else if (action == "clear") {
        return handle_clear(request);
    } else if (action == "stats") {
        return handle_stats(request);
    }
    
    return AgentResult::error("Unknown action: " + action);
}

void RagAgent::shutdown() {
    if (context_) {
        context_->info(name(), "Shutting down");
    }
    initialized_ = false;
    context_ = nullptr;
    rag_manager_ = nullptr;
}

AgentCapability RagAgent::capabilities() const {
    return AgentCapability::RAG_SEARCH | 
           AgentCapability::EMBEDDING |
           AgentCapability::FILE_READ;
}

bool RagAgent::is_ready() const {
    return initialized_;
}

void RagAgent::set_rag_manager(core::RagManager* manager) {
    rag_manager_ = manager;
}

// ============================================================================
// Обработчики действий
// ============================================================================

AgentResult RagAgent::handle_search(const AgentRequest& request) {
    std::string query = request.query();
    
    if (query.empty()) {
        return AgentResult::error("Query is empty");
    }
    
    int k = request.get_param<int>("k", max_chunks_);
    std::string path_filter = request.get_param<std::string>("path_filter", "");
    
    if (context_) {
        context_->debug(name(), "Search query: '" + query + "', k=" + 
                        std::to_string(k));
    }
    
    if (!rag_manager_) {
        return AgentResult::error("RagManager not available");
    }

    // Perform hybrid search
    auto results = rag_manager_->search_hybrid(query, k, path_filter);

    // Convert results to JSON
    nlohmann::json results_json = nlohmann::json::array();
    for (const auto& chunk : results) {
        nlohmann::json chunk_json;
        chunk_json["content"] = chunk.content;
        chunk_json["document_id"] = chunk.document_id;
        chunk_json["chunk_index"] = chunk.chunk_index;
        chunk_json["file_path"] = chunk.file_path;
        chunk_json["language"] = chunk.language;
        chunk_json["symbol_name"] = chunk.symbol_name;
        results_json.push_back(chunk_json);
    }

    return AgentResult::success({
        {"query", query},
        {"results", results_json},
        {"count", results.size()}
    });
}

AgentResult RagAgent::handle_add_document(const AgentRequest& request) {
    std::string file_path = request.file_path();
    
    if (file_path.empty()) {
        return AgentResult::error("File path is empty");
    }
    
    if (context_) {
        context_->info(name(), "Adding document: " + file_path);
    }
    
    if (!rag_manager_) {
        return AgentResult::error("RagManager not available");
    }

    bool success = rag_manager_->process_document(file_path);
    
    return AgentResult::success({
        {"file_path", file_path},
        {"success", success},
        {"chunks_count", rag_manager_->get_external_chunks_count()}
    });
}

AgentResult RagAgent::handle_list_documents(const AgentRequest& request) {
    (void)request;
    
    if (!rag_manager_) {
        return AgentResult::success({
            {"documents", nlohmann::json::array()},
            {"message", "RagManager not available"}
        });
    }

    // Get profile documents
    auto profile_names = rag_manager_->get_index_profile_names();
    nlohmann::json profiles_json = nlohmann::json::array();
    for (const auto& name : profile_names) {
        profiles_json.push_back(name);
    }

    return AgentResult::success({
        {"profiles", profiles_json},
        {"current_profile", rag_manager_->get_current_index_profile()},
        {"total_chunks", rag_manager_->get_external_chunks_count()}
    });
}

AgentResult RagAgent::handle_clear(const AgentRequest& request) {
    (void)request;
    
    if (context_) {
        context_->info(name(), "Clearing RAG cache");
    }
    
    if (!rag_manager_) {
        return AgentResult::error("RagManager not available");
    }

    rag_manager_->clear_all_indexes();
    
    return AgentResult::success({
        {"cleared", true}
    });
}

AgentResult RagAgent::handle_stats(const AgentRequest& request) {
    (void)request;
    
    nlohmann::json stats;
    stats["initialized"] = initialized_;
    stats["max_chunks"] = max_chunks_;
    stats["similarity_threshold"] = similarity_threshold_;
    stats["cache_enabled"] = cache_enabled_;
    stats["rag_manager_available"] = (rag_manager_ != nullptr);

    if (rag_manager_) {
        stats["external_chunks"] = rag_manager_->get_external_chunks_count();
        stats["chat_history_chunks"] = rag_manager_->get_chat_history_chunks_count();
        stats["current_profile"] = rag_manager_->get_current_index_profile();
        stats["has_persistent_index"] = rag_manager_->has_persistent_index();
    }
    
    return AgentResult::success(stats);
}

} // namespace agents

// ============================================================================
// C-API экспорт для плагина
// ============================================================================

extern "C" {

AGENT_PLUGIN_EXPORT const char* plugin_get_name() {
    return "rag_agent";
}

AGENT_PLUGIN_EXPORT const char* plugin_get_version() {
    return "1.0.0";
}

AGENT_PLUGIN_EXPORT const char* plugin_get_api_version() {
    return AGENT_PLUGIN_API_VERSION;
}

AGENT_PLUGIN_EXPORT agents::IAgent* plugin_create_agent() {
    return new agents::RagAgent();
}

AGENT_PLUGIN_EXPORT void plugin_destroy_agent(agents::IAgent* agent) {
    delete agent;
}

AGENT_PLUGIN_EXPORT PluginExports* plugin_get_exports() {
    static PluginExports exports = {
        AGENT_PLUGIN_API_VERSION,
        plugin_get_name,
        plugin_get_version,
        plugin_get_api_version,
        reinterpret_cast<plugin_create_agent_fn>(plugin_create_agent),
        reinterpret_cast<plugin_destroy_agent_fn>(plugin_destroy_agent),
        nullptr,  // initialize
        nullptr   // shutdown
    };
    return &exports;
}

} // extern "C"
