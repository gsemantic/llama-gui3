#include "../include/core/rag_manager.h"
#include "../include/core/embedding_proxy.h"
#include <iostream>

namespace llama_gui {
namespace core {

bool RagManager::start_embedding_proxy(int port) {
    if (embedding_proxy_ && embedding_proxy_->is_running()) {
        std::cout << "[RAG] Embedding proxy already running on port "
                  << embedding_proxy_->get_port() << std::endl;
        return true;
    }

    // Get the local server URL from settings
    std::string local_server_url = "http://localhost:8081"; // default

    embedding_proxy_ = std::make_unique<EmbeddingProxy>(local_server_url, port);

    embedding_proxy_->set_log_callback([](const std::string& msg) {
        std::cout << msg << std::endl;
    });

    if (embedding_proxy_->start()) {
        std::cout << "[RAG] Embedding proxy started on port " << port
                  << " -> forwarding to " << local_server_url << std::endl;
        return true;
    }

    std::cerr << "[RAG] Failed to start embedding proxy on port " << port << std::endl;
    embedding_proxy_.reset();
    return false;
}

void RagManager::stop_embedding_proxy() {
    if (embedding_proxy_) {
        embedding_proxy_->stop();
        embedding_proxy_.reset();
        std::cout << "[RAG] Embedding proxy stopped" << std::endl;
    }
}

bool RagManager::is_embedding_proxy_running() const {
    return embedding_proxy_ && embedding_proxy_->is_running();
}

} // namespace core
} // namespace llama_gui
