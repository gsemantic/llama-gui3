#include "../include/core/rag_manager.h"
#include <iostream>
#include <chrono>

namespace llama_gui {
namespace core {

void RagManager::set_indexing_progress_callback(IndexingProgressCallback callback) {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    indexing_progress_callback_ = std::move(callback);
}

IndexingProgress RagManager::get_current_indexing_progress() const {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    return current_indexing_progress_;
}

void RagManager::report_indexing_progress(IndexingPhase phase,
                                          const std::string& file,
                                          int file_index,
                                          int total_files,
                                          int chunk,
                                          int total_chunks) {
    std::lock_guard<std::mutex> lock(progress_mutex_);

    current_indexing_progress_.phase = phase;
    current_indexing_progress_.current_file = file;
    current_indexing_progress_.current_file_index = file_index;
    current_indexing_progress_.total_files = total_files;

    if (chunk >= 0) current_indexing_progress_.current_chunk = chunk;
    if (total_chunks >= 0) current_indexing_progress_.total_chunks = total_chunks;

    // Calculate elapsed time
    auto now = std::chrono::steady_clock::now();
    if (current_indexing_progress_.start_time == std::chrono::steady_clock::time_point{}) {
        current_indexing_progress_.start_time = now;
    }
    current_indexing_progress_.elapsed_seconds =
        std::chrono::duration<double>(now - current_indexing_progress_.start_time).count();

    // Calculate overall progress
    if (total_files > 0) {
        float file_progress = static_cast<float>(file_index) / total_files;
        if (total_chunks > 0) {
            float chunk_progress = static_cast<float>(chunk) / total_chunks;
            current_indexing_progress_.overall_progress =
                (file_progress + chunk_progress / total_files);
        } else {
            current_indexing_progress_.overall_progress = file_progress;
        }
    }

    // Build status message
    switch (phase) {
        case IndexingPhase::Parsing:
            current_indexing_progress_.status_message =
                "Parsing: " + file + " (" + std::to_string(file_index + 1) +
                "/" + std::to_string(total_files) + ")";
            break;
        case IndexingPhase::Chunking:
            current_indexing_progress_.status_message =
                "Chunking: " + file;
            break;
        case IndexingPhase::Embedding:
            current_indexing_progress_.status_message =
                "Embedding: " + file + " (" + std::to_string(chunk) +
                "/" + std::to_string(total_chunks) + ")";
            break;
        case IndexingPhase::IndexingFAISS:
            current_indexing_progress_.status_message =
                "Indexing: " + file;
            break;
        case IndexingPhase::Saving:
            current_indexing_progress_.status_message = "Saving index...";
            break;
        default:
            break;
    }

    if (indexing_progress_callback_) {
        indexing_progress_callback_(current_indexing_progress_);
    }
}

void RagManager::report_indexing_complete(int file_count, int chunk_count) {
    std::lock_guard<std::mutex> lock(progress_mutex_);

    current_indexing_progress_.phase = IndexingPhase::Complete;
    current_indexing_progress_.final_file_count = file_count;
    current_indexing_progress_.final_chunk_count = chunk_count;
    current_indexing_progress_.overall_progress = 1.0f;
    current_indexing_progress_.current_file_index = current_indexing_progress_.total_files;

    auto now = std::chrono::steady_clock::now();
    current_indexing_progress_.elapsed_seconds =
        std::chrono::duration<double>(now - current_indexing_progress_.start_time).count();

    current_indexing_progress_.status_message =
        "Complete: " + std::to_string(file_count) + " files, " +
        std::to_string(chunk_count) + " chunks indexed in " +
        std::to_string(static_cast<int>(current_indexing_progress_.elapsed_seconds)) + "s";

    if (indexing_progress_callback_) {
        indexing_progress_callback_(current_indexing_progress_);
    }
}

void RagManager::report_indexing_error(const std::string& error) {
    std::lock_guard<std::mutex> lock(progress_mutex_);

    current_indexing_progress_.phase = IndexingPhase::Error;
    current_indexing_progress_.error_message = error;
    current_indexing_progress_.status_message = "Error: " + error;

    if (indexing_progress_callback_) {
        indexing_progress_callback_(current_indexing_progress_);
    }
}

} // namespace core
} // namespace llama_gui
