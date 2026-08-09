#include "../include/core/rag_manager.h"
#include "../include/core/embedding_generator.h"
#include "../include/core/document_parser.h"
#include "../include/core/llama_interface.h"
#include "../include/core/llama_interface_impl.h"
#include <fstream>
#include <iostream>

#ifdef USE_FAISS
#include <faiss/IndexFlat.h>
#endif

namespace llama_gui {
namespace core {

bool RagManager::process_document(const std::string& file_path, bool report_completion) {
    // Проверяем, что путь к файлу не пуст
    if (file_path.empty()) {
        std::cerr << "Error: Empty file path provided for document processing" << std::endl;
        report_indexing_error("Empty file path");
        return false;
    }

    // Проверяем существование файла
    std::ifstream file(file_path);
    if (!file.good()) {
        std::cerr << "Error: Document file does not exist or is not accessible: " << file_path << std::endl;
        report_indexing_error("File not found: " + file_path);
        return false;
    }
    file.close();

    // Initialize start_time for single-file mode (batch mode sets this in process_documents_batch)
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        if (current_indexing_progress_.start_time == std::chrono::steady_clock::time_point{}) {
            current_indexing_progress_.start_time = std::chrono::steady_clock::now();
        }
        current_indexing_progress_.total_files = 1;
    }

    // Report: Parsing phase
    report_indexing_progress(IndexingPhase::Parsing, file_path, 0, 1);

    // Определяем тип контента и язык
    auto content_type = DocumentParser::detect_content_type(file_path);
    std::string language = DocumentParser::get_language(file_path);

    // Генерируем document_id
    std::string doc_id = generate_doc_id(file_path);

    // Используем DocumentParser для обработки различных форматов
    auto document_parts = DocumentParser::parse_document(file_path);

    if (document_parts.empty()) {
        std::cerr << "Error: Could not parse document " << file_path << std::endl;
        return false;
    }

    // === KV-CACHE: Сохраняем полный текст документа в KV-cache ===
    if (enable_kv_cache_) {
        // Проверяем, есть уже сохранённый KV-cache
        if (has_document_kv_cache(doc_id)) {
            // KV-cache уже существует
        } else {
            // Собираем полный текст документа
            std::string full_text;
            full_text.reserve(10000);
            for (const auto& part : document_parts) {
                if (!part.empty()) {
                    full_text += part + "\n";
                }
            }

            // Загружаем текст в слот для токенизации и сохраняем KV-cache
            // Используем слот 0 (резервируем слоты 1-3 для чата)
            int slot_id = 0;
            auto tokenize_result = llama_interface_->tokenize_text_in_slot(slot_id, full_text);

            if (tokenize_result.success) {
                // Сохраняем KV-cache
                if (save_document_kv_cache(doc_id, slot_id)) {
                    doc_id_to_slot_map_[doc_id] = slot_id;
                }

                // Сбрасываем слот после сохранения
                llama_interface_->reset_slot(slot_id);
            }
        }
    }
    // === КОНЕЦ KV-CACHE ===

    // Обрабатываем каждый фрагмент документа для RAG-поиска
    int total_chunks = 0;
    size_t chunks_before = external_chunks_.size();

    // First pass: count total chunks for progress reporting
    int estimated_total_chunks = 0;
    for (size_t i = 0; i < document_parts.size(); ++i) {
        if (!document_parts[i].empty()) {
            auto chunks = split_into_chunks(document_parts[i], max_tokens_per_chunk_);
            estimated_total_chunks += static_cast<int>(chunks.size());
        }
    }

    int processed_chunks = 0;

    for (size_t i = 0; i < document_parts.size(); ++i) {
        // Проверяем, что фрагмент не пуст
        if (document_parts[i].empty()) {
            continue;
        }

        // Report: Chunking phase
        report_indexing_progress(IndexingPhase::Chunking, file_path, 0, 1,
                                 static_cast<int>(i), static_cast<int>(document_parts.size()));

        // Разбиваем каждый фрагмент на чанки, если он слишком большой
        // Используем max_tokens_per_chunk из настроек (по умолчанию 2048)
        auto chunks = split_into_chunks(document_parts[i], max_tokens_per_chunk_);

        if (chunks.empty()) {
            continue;
        }

        for (size_t j = 0; j < chunks.size(); ++j) {
            // Report: Embedding phase
            report_indexing_progress(IndexingPhase::Embedding, file_path, 0, 1,
                                     processed_chunks, estimated_total_chunks);

            if (!process_text_chunk(chunks[j], file_path, static_cast<int>(i * 1000 + j))) {
                // Продолжаем обработку остальных чанков
            } else {
                total_chunks++;
            }
            processed_chunks++;
        }
    }

    // Set code-aware metadata on newly added chunks
    for (size_t idx = chunks_before; idx < external_chunks_.size(); ++idx) {
        auto& chunk = external_chunks_[idx];
        chunk.content_type = static_cast<RagChunk::ContentType>(static_cast<int>(content_type));
        chunk.language = language;
        chunk.file_path = file_path;

        // Parse [[start:end:name:parent]] prefix from AST chunks
        if (chunk.content.size() > 4 && chunk.content[0] == '[' && chunk.content[1] == '[') {
            size_t close = chunk.content.find("]]", 2);
            if (close != std::string::npos) {
                std::string meta = chunk.content.substr(2, close - 2);
                // Format: start:end:name:parent (or start:end:name for old format)
                size_t p1 = meta.find(':');
                size_t p2 = (p1 != std::string::npos) ? meta.find(':', p1 + 1) : std::string::npos;
                size_t p3 = (p2 != std::string::npos) ? meta.find(':', p2 + 1) : std::string::npos;

                if (p1 != std::string::npos && p2 != std::string::npos) {
                    chunk.start_line = std::stoi(meta.substr(0, p1));
                    chunk.end_line = std::stoi(meta.substr(p1 + 1, p2 - p1 - 1));
                    chunk.symbol_name = meta.substr(p2 + 1, (p3 != std::string::npos ? p3 : meta.size()) - p2 - 1);
                    if (p3 != std::string::npos) {
                        chunk.parent_symbol = meta.substr(p3 + 1);
                    }
                    // Strip prefix from content
                    chunk.content = chunk.content.substr(close + 2);
                } else if (p1 != std::string::npos) {
                    // Old format: end_line:symbol_name (no start_line, no parent)
                    chunk.end_line = std::stoi(meta.substr(0, p1));
                    chunk.symbol_name = meta.substr(p1 + 1);
                    chunk.content = chunk.content.substr(close + 2);
                }
            }
        }
    }

    // Добавляем документ в текущий профиль
    profile_manager_.add_document_to_current_profile(file_path);
    profile_manager_.update_current_profile_chunk_count(static_cast<int>(external_chunks_.size()));

    // Report: Saving phase
    report_indexing_progress(IndexingPhase::Saving, file_path, 1, 1);

    // === ПЕРСИСТЕНТНОСТЬ: Автосохранение индекса после обработки документа ===
    save_index();

    // Report completion (only in single-file mode; batch mode handles this separately)
    if (report_completion) {
        if (total_chunks > 0) {
            report_indexing_complete(1, total_chunks);
        } else {
            report_indexing_error("No chunks generated from document");
        }
    }

    return total_chunks > 0;
}

bool RagManager::process_text_chunk(const std::string& text, const std::string& doc_id, int chunk_index) {
    // Проверяем, что текст не пуст
    if (text.empty()) {
        std::cerr << "Warning: Empty text provided for chunk processing" << std::endl;
        return false;
    }

    // Проверяем наличие индекса - если нет, создаем
#ifdef USE_FAISS
    if (!external_docs_index_) {
        external_docs_index_ = create_optimized_index(EMBEDDING_DIMENSION);
        if (!external_docs_index_) {
            std::cerr << "Error: Failed to create FAISS index" << std::endl;
            return false;
        }
    }
#endif

    // Проверяем ограничение на размер
    cleanup_old_chunks();

    RagChunk chunk;
    chunk.content = text;
    chunk.document_id = doc_id;
    chunk.chunk_index = chunk_index;

    // Генерируем эмбеддинг
    chunk.embedding = generate_embedding(text);

    // Проверяем, что эмбеддинг был успешно сгенерирован
    if (chunk.embedding.empty()) {
        std::cerr << "Error: Failed to generate embedding for chunk " << chunk_index << " of document " << doc_id << std::endl;
        return false;
    }

    // Добавляем в FAISS индекс
#ifdef USE_FAISS
    if (external_docs_index_) {
        // Преобразуем вектор в формат FAISS
        std::vector<float> vector_for_faiss = chunk.embedding;

        // Проверяем, что размер вектора соответствует размерности индекса.
        // Если нет - пересоздаём индекс под фактическую размерность эмбеддинга,
        // чтобы индексация не молча отбрасывала все чанки (когда сервер меняет
        // размерность модели эмбеддингов).
        if (vector_for_faiss.size() != external_docs_index_->d) {
            std::cerr << "[RAG] Embedding dimension changed from "
                      << external_docs_index_->d << " to " << vector_for_faiss.size()
                      << ", rebuilding index" << std::endl;
            external_docs_index_ = create_optimized_index(static_cast<int>(vector_for_faiss.size()));
            // Векторы прежней размерности несовместимы с новым индексом
            external_chunks_.clear();
            if (!external_docs_index_) {
                std::cerr << "Error: Failed to recreate FAISS index for dimension "
                          << vector_for_faiss.size() << std::endl;
                return false;
            }
        }

        // Нормализуем вектор для косинусного сходства
        normalize_vector(vector_for_faiss);

        // Добавляем в индекс - используем текущий размер КАК ID (перед добавлением)
        try {
            external_docs_index_->add(1, vector_for_faiss.data());
        } catch (const std::exception& e) {
            std::cerr << "Error: Failed to add vector to FAISS index: " << e.what() << std::endl;
            return false;
        }
    } else {
        std::cerr << "Error: external_docs_index_ is null, cannot add chunk" << std::endl;
        return false;
    }
#endif

    external_chunks_.push_back(chunk);
    return true;
}

bool RagManager::process_documents_batch(const std::vector<std::string>& file_paths) {
    if (file_paths.empty()) return false;

    int total_files = static_cast<int>(file_paths.size());
    int total_chunks_all = 0;
    bool any_success = false;

    // Reset progress tracking
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        current_indexing_progress_ = IndexingProgress{};
        current_indexing_progress_.total_files = total_files;
        current_indexing_progress_.start_time = std::chrono::steady_clock::now();
    }

    report_indexing_progress(IndexingPhase::Parsing, file_paths[0], 0, total_files);

    for (int i = 0; i < total_files; ++i) {
        size_t chunks_before = external_chunks_.size();

        // Report per-file progress
        report_indexing_progress(IndexingPhase::Parsing, file_paths[i], i, total_files);

        bool success = process_document(file_paths[i], false);

        if (success) {
            int file_chunks = static_cast<int>(external_chunks_.size()) - static_cast<int>(chunks_before);
            total_chunks_all += file_chunks;
            any_success = true;
        }
    }

    // Report final completion with correct totals
    if (any_success) {
        report_indexing_complete(total_files, total_chunks_all);
    } else {
        report_indexing_error("No chunks generated from any document");
    }

    return total_chunks_all > 0;
}

bool RagManager::reindex_document(const std::string& file_path) {
    // Remove old chunks for this file, then re-add
    remove_document(file_path);
    return process_document(file_path);
}

bool RagManager::remove_document(const std::string& file_path) {
    std::string doc_id = generate_doc_id(file_path);

    // Remove chunks from external_chunks_
    size_t before = external_chunks_.size();
    external_chunks_.erase(
        std::remove_if(external_chunks_.begin(), external_chunks_.end(),
            [&](const RagChunk& chunk) {
                return chunk.document_id == doc_id || chunk.file_path == file_path;
            }),
        external_chunks_.end());

    size_t removed = before - external_chunks_.size();
    if (removed == 0) {
        return false;
    }

    std::cout << "[RAG] Removed " << removed << " chunks for document: " << file_path << std::endl;

    // Rebuild FAISS index from remaining chunks
    rebuild_index();

    // Delete KV-cache if exists
    delete_document_kv_cache(doc_id);

    return true;
}

} // namespace core
} // namespace llama_gui
