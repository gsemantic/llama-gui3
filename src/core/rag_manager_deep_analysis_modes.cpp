#include "../include/core/rag_manager.h"
#include "../include/core/rag_deep_analysis_utils.h"
#include <iostream>
#include <algorithm>
#include <chrono>

namespace llama_gui {
namespace core {

using namespace deep_analysis;

// ============================================================================
// ИТЕРАТИВНЫЙ РЕЖИМ
// ============================================================================

std::string RagManager::process_deep_analysis_iterative(
    const std::string& query,
    std::vector<RagChunk>& all_chunks,
    const DeepAnalysisSettings& settings)
{
    std::cout << "\n[ITERATIVE] === Starting iterative chunk processing ===" << std::endl;

    auto start_time = std::chrono::steady_clock::now();

    std::string accumulated_summary;
    accumulated_summary.reserve(20000);

    int processed_count = 0;

    for (size_t i = 0; i < all_chunks.size(); ++i) {
        if (processed_count >= settings.max_iterations) {
            std::cout << "[ITERATIVE] Reached max_iterations limit ("
                      << settings.max_iterations << ") at chunk " << i << std::endl;
            break;
        }

        std::cout << "\n[ITERATIVE] Processing chunk " << (i+1) << "/"
                  << all_chunks.size() << " (doc: " << all_chunks[i].document_id
                  << ", idx: " << all_chunks[i].chunk_index << ")..." << std::endl;

        std::string chunk_summary = generate_chunk_summary(all_chunks[i], query);

        if (!chunk_summary.empty()) {
            if (!accumulated_summary.empty()) {
                accumulated_summary += "\n\n";
            }
            accumulated_summary += "=== Chunk " + std::to_string(i+1) + " ===\n";
            accumulated_summary += chunk_summary;

            processed_count++;

            if (settings.enable_progressive_summary &&
                estimate_tokens(accumulated_summary) > settings.target_context_size / 2) {

                std::cout << "[ITERATIVE] Accumulated summary is large, compressing..." << std::endl;

                RagChunk summary_chunk;
                summary_chunk.content = accumulated_summary;
                summary_chunk.document_id = "accumulated";
                summary_chunk.chunk_index = 0;

                std::string compressed = generate_chunk_summary(summary_chunk, query);

                if (!compressed.empty()) {
                    accumulated_summary = compressed;
                    std::cout << "[ITERATIVE] Compressed to " << accumulated_summary.size()
                              << " chars (~" << estimate_tokens(accumulated_summary) << " tokens)" << std::endl;
                }
            }

            std::cout << "[ITERATIVE] Current accumulated summary: "
                      << accumulated_summary.size() << " chars" << std::endl;
        } else {
            std::cerr << "[ITERATIVE] Warning: Failed to generate summary for chunk " << i << std::endl;
        }
    }

    std::cout << "\n[ITERATIVE] === Final synthesis ===" << std::endl;

    std::vector<std::string> summaries = {accumulated_summary};
    std::string final_answer = synthesize_final_answer(
        query,
        summaries,
        settings.target_context_size);

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();

    std::cout << "\n[ITERATIVE] Completed in " << format_duration(duration) << std::endl;
    std::cout << "[ITERATIVE] Processed " << processed_count << " chunks" << std::endl;
    std::cout << "[ITERATIVE] Final answer: " << final_answer.size() << " chars" << std::endl;

    return final_answer;
}

// ============================================================================
// ИЕРАРХИЧЕСКИЙ РЕЖИМ
// ============================================================================

std::string RagManager::process_deep_analysis_hierarchical(
    const std::string& query,
    std::vector<RagChunk>& all_chunks,
    const DeepAnalysisSettings& settings)
{
    std::cout << "\n[HIERARCHICAL] === Building summary tree ===" << std::endl;

    auto start_time = std::chrono::steady_clock::now();

    std::vector<std::string> current_level;
    current_level.reserve(all_chunks.size());

    for (const auto& chunk : all_chunks) {
        current_level.push_back(chunk.content);
    }

    std::cout << "[HIERARCHICAL] Level 0: " << current_level.size() << " leaf chunks" << std::endl;

    int level = 0;

    while (current_level.size() > 1) {
        level++;
        std::cout << "\n[HIERARCHICAL] === Building level " << level << " ===" << std::endl;

        int group_size = std::max(2, settings.chunks_per_batch / 2);
        auto batches = create_batches_for_strings(current_level, group_size);

        std::vector<std::string> next_level;
        next_level.reserve(batches.size());

        int group_num = 0;
        for (const auto& batch : batches) {
            group_num++;

            std::string group_text;
            group_text.reserve(15000);

            for (size_t i = 0; i < batch.size(); ++i) {
                group_text += "\n--- Item " + std::to_string(i+1) + " ---\n";
                group_text += batch[i];
            }

            RagChunk group_chunk;
            group_chunk.content = group_text;
            group_chunk.document_id = "level_" + std::to_string(level) + "_group_" + std::to_string(group_num);
            group_chunk.chunk_index = 0;

            std::string summary = generate_chunk_summary(group_chunk, query);

            if (!summary.empty()) {
                next_level.push_back(summary);
                std::cout << "[HIERARCHICAL] Group " << group_num << " -> "
                          << summary.size() << " chars" << std::endl;
            }
        }

        if (next_level.empty()) {
            std::cerr << "[HIERARCHICAL] Error: Failed to generate any summaries at level "
                      << level << std::endl;
            break;
        }

        current_level = std::move(next_level);
        std::cout << "[HIERARCHICAL] Level " << level << ": " << current_level.size() << " summaries" << std::endl;

        if (level > 20) {
            std::cerr << "[HIERARCHICAL] Warning: Too many levels, stopping" << std::endl;
            break;
        }
    }

    std::cout << "\n[HIERARCHICAL] === Final synthesis from root ===" << std::endl;

    std::string final_answer;

    if (current_level.size() == 1) {
        final_answer = current_level[0];
    } else {
        std::vector<std::string> summaries = current_level;
        final_answer = synthesize_final_answer(query, summaries, settings.target_context_size);
    }

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();

    std::cout << "\n[HIERARCHICAL] Completed in " << format_duration(duration) << std::endl;
    std::cout << "[HIERARCHICAL] Tree depth: " << level << " levels" << std::endl;
    std::cout << "[HIERARCHICAL] Final answer: " << final_answer.size() << " chars" << std::endl;

    return final_answer;
}

} // namespace core
} // namespace llama_gui
