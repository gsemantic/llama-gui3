#include "wp_rag_agent.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <core/rag_manager.h>
#include <core/embedding_server.h>
#include <nlohmann/json.hpp>

namespace wp_coder {

// ===========================================================================
// WPRagAgent implementation
// ===========================================================================

WPRagAgent::WPRagAgent() = default;

WPRagAgent::~WPRagAgent() {
    shutdown();
}

const char* WPRagAgent::name() const {
    return "wp_rag_agent";
}

const char* WPRagAgent::description() const {
    return "WordPress RAG agent. Provides semantic search over WordPress codebase (core, themes, plugins).";
}

const char* WPRagAgent::version() const {
    return "0.1.0";
}

bool WPRagAgent::initialize(agents::AgentContext* context) {
    if (!context) {
        return false;
    }
    
    context_ = context;
    initialized_ = true;
    
    // Инициализация RagManager
    rag_manager_ = std::make_unique<llama_gui::core::RagManager>();
    
    // Загрузка настроек из конфигурации
    if (context_) {
        auto config = context_->get_agent_config(name());
        if (config.contains("embedding_model")) {
            rag_manager_ = std::make_unique<llama_gui::core::RagManager>(
                config["embedding_model"].get<std::string>());
        }
        
        context_->info(name(), "Initialized with RagManager");
    }
    
    return true;
}

agents::AgentResult WPRagAgent::execute(const agents::AgentRequest& request) {
    if (!initialized_) {
        return agents::AgentResult::error("Agent not initialized");
    }
    
    std::string action = request.action();
    
    if (action == "search") {
        return handle_search(request);
    } else if (action == "add_corpus") {
        return handle_add_corpus(request);
    } else if (action == "list_corpus") {
        return handle_list_corpus(request);
    } else if (action == "clear") {
        return handle_clear(request);
    } else if (action == "stats") {
        return handle_stats(request);
    }
    
    return agents::AgentResult::error("Unknown action: " + action);
}

void WPRagAgent::shutdown() {
    if (context_) {
        context_->info(name(), "Shutting down");
    }
    initialized_ = false;
    rag_manager_.reset();
    context_ = nullptr;
}

agents::AgentCapability WPRagAgent::capabilities() const {
    return agents::AgentCapability::RAG_SEARCH | 
           agents::AgentCapability::FILE_READ;
}

bool WPRagAgent::is_ready() const {
    return initialized_ && rag_manager_ != nullptr;
}

// ===========================================================================
// Обработчики действий
// ===========================================================================

agents::AgentResult WPRagAgent::handle_search(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Searching WordPress codebase");
    }
    
    if (!rag_manager_) {
        return agents::AgentResult::error("RagManager not initialized");
    }
    
    std::string query = request.get_param<std::string>("query", "");
    if (query.empty()) {
        return agents::AgentResult::error("Query is empty");
    }
    
    int k = request.get_param<int>("k", 5);
    std::string path_filter = request.get_param<std::string>("path_filter", "");
    
    try {
        // Используем гибридный поиск (векторный + полнотекстовый)
        auto results = rag_manager_->search_hybrid(query, k, path_filter);
        
        // Преобразуем результаты в JSON
        nlohmann::json results_json = nlohmann::json::array();
        for (const auto& chunk : results) {
            nlohmann::json chunk_json;
            chunk_json["content"] = chunk.content;
            chunk_json["document_id"] = chunk.document_id;
            chunk_json["chunk_index"] = chunk.chunk_index;
            chunk_json["file_path"] = chunk.file_path;
            chunk_json["language"] = chunk.language;
            chunk_json["symbol_name"] = chunk.symbol_name;
            chunk_json["similarity"] = chunk.embedding.empty() ? 0.0f : 1.0f; // заглушка
            results_json.push_back(chunk_json);
        }
        
        return agents::AgentResult::success({
            {"query", query},
            {"results", results_json},
            {"count", static_cast<int>(results.size())},
            {"status", "success"}
        });
    } catch (const std::exception& e) {
        if (context_) {
            context_->error(name(), "RAG search error: " + std::string(e.what()));
        }
        return agents::AgentResult::error("RAG search failed: " + std::string(e.what()));
    }
}

agents::AgentResult WPRagAgent::handle_add_corpus(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Adding WordPress corpus");
    }
    
    if (!rag_manager_) {
        return agents::AgentResult::error("RagManager not initialized");
    }
    
    std::string path = request.get_param<std::string>("path", "");
    if (path.empty()) {
        return agents::AgentResult::error("Path is empty");
    }
    
    try {
        // TODO: Реальная индексация через process_document
        // Для MVP просто проверяем существование
        std::ifstream file(path);
        if (!file.is_open()) {
            return agents::AgentResult::error("Cannot open file: " + path);
        }
        
        // В реальности здесь будет: rag_manager_->process_document(path);
        
        return agents::AgentResult::success({
            {"path", path},
            {"added", true},
            {"message", "Corpus addition not fully implemented yet"}
        });
    } catch (const std::exception& e) {
        if (context_) {
            context_->error(name(), "Add corpus error: " + std::string(e.what()));
        }
        return agents::AgentResult::error("Add corpus failed: " + std::string(e.what()));
    }
}

agents::AgentResult WPRagAgent::handle_list_corpus(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Listing corpus documents");
    }
    
    if (!rag_manager_) {
        return agents::AgentResult::error("RagManager not initialized");
    }
    
    // TODO: Получить список документов из RagManager
    // Для MVP возвращаем заглушку
    
    nlohmann::json documents = nlohmann::json::array();
    
    return agents::AgentResult::success({
        {"documents", documents},
        {"count", 0},
        {"message", "Corpus listing not fully implemented yet"}
    });
}

agents::AgentResult WPRagAgent::handle_clear(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Clearing corpus");
    }
    
    if (!rag_manager_) {
        return agents::AgentResult::error("RagManager not initialized");
    }
    
    try {
        // TODO: Реальная очистка индекса
        // rag_manager_->clear_index();
        
        return agents::AgentResult::success({
            {"cleared", true},
            {"message", "Corpus clear not fully implemented yet"}
        });
    } catch (const std::exception& e) {
        if (context_) {
            context_->error(name(), "Clear corpus error: " + std::string(e.what()));
        }
        return agents::AgentResult::error("Clear corpus failed: " + std::string(e.what()));
    }
}

agents::AgentResult WPRagAgent::handle_stats(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Getting corpus stats");
    }
    
    if (!rag_manager_) {
        return agents::AgentResult::error("RagManager not initialized");
    }
    
    // TODO: Получить статистику из RagManager
    // Для MVP возвращаем заглушку
    
    return agents::AgentResult::success({
        {"total_documents", 0},
        {"total_chunks", 0},
        {"index_size_mb", 0.0},
        {"message", "Corpus stats not fully implemented yet"}
    });
}

} // namespace wp_coder