#include "../include/core/rag_manager.h"
#include "../include/core/rag_deep_analysis_utils.h"
#include <iostream>
#include <chrono>

namespace llama_gui {
namespace core {

using namespace deep_analysis;

// ============================================================================
// ОСНОВНОЙ МЕТОД ГЛУБОКОГО АНАЛИЗА
// ============================================================================

std::string RagManager::process_deep_analysis(
    const std::string& query,
    std::vector<RagChunk>& all_chunks,
    const DeepAnalysisSettings& settings)
{
    auto start_time = std::chrono::steady_clock::now();

    std::cout << "\n" << std::string(80, '=') << std::endl;
    std::cout << "[DEEP ANALYSIS] Starting deep document analysis" << std::endl;
    std::cout << "[DEEP ANALYSIS] Query: \"" << query.substr(0, 100);
    if (query.size() > 100) std::cout << "...";
    std::cout << "\"" << std::endl;
    std::cout << "[DEEP ANALYSIS] Total chunks: " << all_chunks.size() << std::endl;

    if (!llama_interface_) {
        std::cerr << "[DEEP ANALYSIS] Error: LlamaInterface not initialized" << std::endl;
        std::cerr << "[DEEP ANALYSIS] Make sure the server is running at http://localhost:8081" << std::endl;
        return "Ошибка: Сервер для анализа не запущен. Запустите сервер llama.cpp.";
    }

    try {
        if (!llama_interface_->is_server_healthy()) {
            std::cerr << "[DEEP ANALYSIS] Error: Server is not reachable" << std::endl;
            std::cerr << "[DEEP ANALYSIS] Make sure the server is running at http://localhost:8081" << std::endl;
            return "Ошибка: Сервер недоступен. Запустите сервер llama.cpp.";
        }
    } catch (const std::exception& e) {
        std::cerr << "[DEEP ANALYSIS] Error checking server: " << e.what() << std::endl;
        return "Ошибка проверки сервера: " + std::string(e.what());
    }

    std::cout << "[DEEP ANALYSIS] Mode: ";

    switch (settings.mode) {
        case DeepAnalysisMode::MapReduce:
            std::cout << "MapReduce";
            break;
        case DeepAnalysisMode::Iterative:
            std::cout << "Iterative";
            break;
        case DeepAnalysisMode::Hierarchical:
            std::cout << "Hierarchical";
            break;
        default:
            std::cout << "Disabled";
            return "";
    }

    std::cout << ", chunks_per_batch: " << settings.chunks_per_batch
              << ", max_iterations: " << settings.max_iterations
              << ", target_context_size: " << settings.target_context_size
              << std::endl;
    std::cout << std::string(80, '=') << "\n" << std::endl;

    if (settings.auto_adjust_context_size) {
        std::cout << "[DEEP ANALYSIS] Auto-adjusting server context size to "
                  << settings.target_context_size << "..." << std::endl;
        if (!auto_adjust_server_context_size(settings.target_context_size)) {
            std::cerr << "[DEEP ANALYSIS] Warning: Failed to adjust server context size" << std::endl;
        }
    }

    std::string result;

    switch (settings.mode) {
        case DeepAnalysisMode::MapReduce:
            result = process_deep_analysis_mapreduce(query, all_chunks, settings);
            break;

        case DeepAnalysisMode::Iterative:
            result = process_deep_analysis_iterative(query, all_chunks, settings);
            break;

        case DeepAnalysisMode::Hierarchical:
            result = process_deep_analysis_hierarchical(query, all_chunks, settings);
            break;

        default:
            std::cerr << "[DEEP ANALYSIS] Error: Unknown deep analysis mode" << std::endl;
            return "";
    }

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();

    std::cout << "\n" << std::string(80, '=') << std::endl;
    std::cout << "[DEEP ANALYSIS] Completed in " << format_duration(duration) << std::endl;
    std::cout << "[DEEP ANALYSIS] Result size: " << result.size() << " chars"
              << " (~" << estimate_tokens(result) << " tokens)" << std::endl;
    std::cout << std::string(80, '=') << "\n" << std::endl;

    return result;
}

} // namespace core
} // namespace llama_gui
