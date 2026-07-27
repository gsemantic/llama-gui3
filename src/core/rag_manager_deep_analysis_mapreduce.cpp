#include "../include/core/rag_manager.h"
#include "../include/core/rag_deep_analysis_utils.h"
#include <iostream>
#include <chrono>
#include <future>

namespace llama_gui {
namespace core {

using namespace deep_analysis;

// ============================================================================
// MAP-REDUCE РЕЖИМ
// ============================================================================

std::string RagManager::process_deep_analysis_mapreduce(
    const std::string& query,
    std::vector<RagChunk>& all_chunks,
    const DeepAnalysisSettings& settings)
{
    std::cout << "\n[MAP-REDUCE] === PHASE 1: MAP (Creating batch summaries) ===" << std::endl;

    auto map_start = std::chrono::steady_clock::now();

    int batch_size = settings.chunks_per_batch;
    auto batches = create_batches(all_chunks, batch_size);

    std::cout << "[MAP-REDUCE] Split " << all_chunks.size() << " chunks into "
              << batches.size() << " batches (batch_size=" << batch_size << ")" << std::endl;

    // Use sequential processing (1 at a time) to avoid overwhelming local server
    // Local CPU models are slow - parallel requests cause slot exhaustion and timeouts
    const int max_concurrent = 1;
    const int num_threads = std::min(max_concurrent, static_cast<int>(batches.size()));
    std::cout << "[MAP-REDUCE] Using " << num_threads << " concurrent workers for " << batches.size() << " batches" << std::endl;

    std::vector<std::string> batch_summaries(batches.size());

    // Process in waves of max_concurrent
    for (size_t wave_start = 0; wave_start < batches.size(); wave_start += max_concurrent) {
        size_t wave_end = std::min(wave_start + static_cast<size_t>(max_concurrent), batches.size());
        std::vector<std::future<void>> futures;
        futures.reserve(wave_end - wave_start);

        for (size_t batch_idx = wave_start; batch_idx < wave_end; ++batch_idx) {
            futures.push_back(std::async(std::launch::async, [this, batch_idx, &batch_summaries, &batches, &query]() {
                const auto& batch = batches[batch_idx];

                std::string batch_text;
                batch_text.reserve(10000);

                for (size_t i = 0; i < batch.size(); ++i) {
                    batch_text += "\n--- Chunk " + std::to_string(i+1) + " (doc: " + batch[i].document_id
                                  + ", idx: " + std::to_string(batch[i].chunk_index) + ") ---\n";
                    batch_text += batch[i].content;
                }

                RagChunk batch_chunk;
                batch_chunk.content = batch_text;
                batch_chunk.document_id = "batch_" + std::to_string(batch_idx + 1);
                batch_chunk.chunk_index = 0;

                std::string summary = generate_chunk_summary(batch_chunk, query);

                if (!summary.empty()) {
                    batch_summaries[batch_idx] = std::move(summary);
                }
            }));
        }

        for (auto& future : futures) {
            future.get();
        }
    }

    std::vector<std::string> filtered_summaries;
    filtered_summaries.reserve(batch_summaries.size());

    for (size_t i = 0; i < batch_summaries.size(); ++i) {
        if (!batch_summaries[i].empty()) {
            filtered_summaries.push_back(std::move(batch_summaries[i]));

            if (static_cast<int>(filtered_summaries.size()) >= settings.max_iterations) {
                std::cout << "[MAP-REDUCE] Reached max_iterations limit ("
                          << settings.max_iterations << ")" << std::endl;
                break;
            }
        } else {
            std::cerr << "[MAP-REDUCE] Warning: Failed to generate summary for batch " << (i + 1) << std::endl;
        }
    }

    batch_summaries = std::move(filtered_summaries);

    auto map_end = std::chrono::steady_clock::now();
    auto map_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        map_end - map_start).count();

    std::cout << "\n[MAP-REDUCE] MAP phase completed in " << format_duration(map_duration) << std::endl;
    std::cout << "[MAP-REDUCE] Generated " << batch_summaries.size() << " batch summaries" << std::endl;

    std::cout << "\n[MAP-REDUCE] === PHASE 2: REDUCE (Synthesizing final answer) ===" << std::endl;

    auto reduce_start = std::chrono::steady_clock::now();

    std::string final_answer = synthesize_final_answer(
        query,
        batch_summaries,
        settings.target_context_size);

    auto reduce_end = std::chrono::steady_clock::now();
    auto reduce_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        reduce_end - reduce_start).count();

    std::cout << "\n[MAP-REDUCE] REDUCE phase completed in " << format_duration(reduce_duration) << std::endl;
    std::cout << "[MAP-REDUCE] Final answer: " << final_answer.size() << " chars"
              << " (~" << estimate_tokens(final_answer) << " tokens)" << std::endl;

    return final_answer;
}

} // namespace core
} // namespace llama_gui
