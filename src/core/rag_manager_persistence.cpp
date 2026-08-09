#include "../include/core/rag_manager.h"
#include "../include/core/embedding_generator.h"
#include <fstream>
#include <iostream>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
#include <nlohmann/json.hpp>
#include <filesystem>

#ifdef USE_FAISS
#include <faiss/IndexFlat.h>
#include <faiss/index_io.h>
#endif

namespace llama_gui {
namespace core {

std::string RagManager::get_default_index_path() const {
    // Сохраняем индекс в домашней директории пользователя
    const char* home = getenv("HOME");
    if (!home) {
        home = ".";
    }

    std::string index_dir = std::string(home) + "/.llama-gui/rag_indexes/";

    // Создаём директорию если не существует (создаём всё дерево директорий)
    // Используем system с mkdir -p для надёжности
    std::string mkdir_cmd = "mkdir -p \"" + index_dir + "\"";
    int result = system(mkdir_cmd.c_str());
    (void)result; // Игнорируем результат - директория может уже существовать

    return index_dir + "rag_index.faiss";
}

bool RagManager::has_persistent_index() const {
    // Сначала пробуем получить путь из текущего профиля
    std::string index_path = profile_manager_.get_current_index_path();
    
    // Если профиль не установлен или путь пустой, используем путь по умолчанию
    if (index_path.empty()) {
        index_path = get_default_index_path();
    }
    
    std::string metadata_path = index_path + ".metadata.json";

    struct stat buffer;
    bool index_exists = (stat(index_path.c_str(), &buffer) == 0);
    bool metadata_exists = (stat(metadata_path.c_str(), &buffer) == 0);

    return index_exists && metadata_exists;
}

bool RagManager::save_index(const std::string& index_path) {
    // Если путь не указан, используем путь из текущего профиля
    std::string path = index_path.empty() ? profile_manager_.get_current_index_path() : index_path;
    
    // Если профиль не установлен или путь пустой, используем путь по умолчанию
    if (path.empty()) {
        path = get_default_index_path();
    }
    
    std::string metadata_path = path + ".metadata.json";

    std::cout << "[RAG PERSISTENCE] Saving index to: " << path << std::endl;
    std::cout << "[RAG PERSISTENCE] Metadata to: " << metadata_path << std::endl;

#ifdef USE_FAISS
    if (!external_docs_index_ || external_chunks_.empty()) {
        std::cerr << "[RAG PERSISTENCE] Error: No index or chunks to save" << std::endl;
        return false;
    }

    try {
        // Сохраняем FAISS индекс
        faiss::write_index(external_docs_index_.get(), path.c_str());
        std::cout << "[RAG PERSISTENCE] FAISS index saved successfully" << std::endl;

        // Сохраняем метаданные чанков в JSON
        nlohmann::json metadata;
        metadata["version"] = 3;  // Bumped: хранится embedding_model для проверки совместимости
        metadata["chunk_count"] = external_chunks_.size();
        // Используем фактическую размерность индекса, а не жёстко заданную константу
        metadata["embedding_dimension"] = external_docs_index_
            ? static_cast<int>(external_docs_index_->d)
            : EMBEDDING_DIMENSION;
        // Модель, которой построен индекс (basename пути) — для проверки при загрузке
        metadata["embedding_model"] = embedding_model_path_.empty()
            ? "unknown"
            : std::filesystem::path(embedding_model_path_).filename().string();

        // Сохраняем информацию о каждом чанке (без эмбеддингов - они в индексе)
        nlohmann::json chunks_json = nlohmann::json::array();
        for (const auto& chunk : external_chunks_) {
            nlohmann::json chunk_meta;
            chunk_meta["content"] = chunk.content;
            chunk_meta["document_id"] = chunk.document_id;
            chunk_meta["chunk_index"] = chunk.chunk_index;
            // Code-aware metadata
            chunk_meta["content_type"] = static_cast<int>(chunk.content_type);
            if (!chunk.language.empty()) chunk_meta["language"] = chunk.language;
            if (!chunk.file_path.empty()) chunk_meta["file_path"] = chunk.file_path;
            if (!chunk.symbol_name.empty()) chunk_meta["symbol_name"] = chunk.symbol_name;
            if (!chunk.parent_symbol.empty()) chunk_meta["parent_symbol"] = chunk.parent_symbol;
            if (chunk.start_line > 0) chunk_meta["start_line"] = chunk.start_line;
            if (chunk.end_line > 0) chunk_meta["end_line"] = chunk.end_line;
            chunks_json.push_back(chunk_meta);
        }

        metadata["chunks"] = chunks_json;

        // Сериализуем в строку ПЕРЕД открытием файла:
        // - error_handler_t::replace заменяет невалидные UTF-8 байты на U+FFFD
        //   вместо выброса исключения type_error.316 (иначе остаётся пустой файл)
        // - Если бы dump() бросил исключение, файл с пустым содержимым не создался бы
        std::string metadata_str;
        try {
            metadata_str = metadata.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
        } catch (const std::exception& e) {
            std::cerr << "[RAG PERSISTENCE] Error: Failed to serialize metadata: " << e.what() << std::endl;
            std::remove(metadata_path.c_str());
            return false;
        }

        // Записываем JSON
        std::ofstream meta_file(metadata_path);
        if (!meta_file.is_open()) {
            std::cerr << "[RAG PERSISTENCE] Error: Cannot open metadata file" << std::endl;
            return false;
        }

        meta_file << metadata_str;
        meta_file.close();

        std::cout << "[RAG PERSISTENCE] Metadata saved successfully ("
                  << external_chunks_.size() << " chunks)" << std::endl;

        // Обновляем информацию в профиле
        profile_manager_.update_current_profile_chunk_count(static_cast<int>(external_chunks_.size()));

        return true;

    } catch (const std::exception& e) {
        std::cerr << "[RAG PERSISTENCE] Error: Failed to save index: " << e.what() << std::endl;
        return false;
    }
#else
    std::cerr << "[RAG PERSISTENCE] Error: FAISS not available" << std::endl;
    return false;
#endif
}

bool RagManager::load_index(const std::string& index_path) {
    std::string path = index_path.empty() ? get_default_index_path() : index_path;
    std::string metadata_path = path + ".metadata.json";

    std::cout << "[RAG PERSISTENCE] Loading index from: " << path << std::endl;
    std::cout << "[RAG PERSISTENCE] Metadata from: " << metadata_path << std::endl;

    // Проверяем существование файлов
    struct stat buffer;
    if (stat(path.c_str(), &buffer) != 0) {
        std::cout << "[RAG PERSISTENCE] Index file does not exist" << std::endl;
        return false;
    }

    if (stat(metadata_path.c_str(), &buffer) != 0) {
        std::cout << "[RAG PERSISTENCE] Metadata file does not exist" << std::endl;
        return false;
    }

#ifdef USE_FAISS
    try {
        // Загружаем метаданные
        std::ifstream meta_file(metadata_path);
        if (!meta_file.is_open()) {
            std::cerr << "[RAG PERSISTENCE] Error: Cannot open metadata file" << std::endl;
            return false;
        }

        nlohmann::json metadata = nlohmann::json::parse(meta_file);
        meta_file.close();

        int chunk_count = metadata.value("chunk_count", 0);
        int embedding_dim = metadata.value("embedding_dimension", EMBEDDING_DIMENSION);
        std::string index_model = metadata.value("embedding_model", "");
        int metadata_version = metadata.value("version", 0);

        std::cout << "[RAG PERSISTENCE] Metadata loaded: " << chunk_count << " chunks, "
                  << embedding_dim << " dimensions" << std::endl;

        // Загружаем FAISS индекс
        external_docs_index_ = std::unique_ptr<faiss::Index>(faiss::read_index(path.c_str(), 0));

        if (!external_docs_index_) {
            std::cerr << "[RAG PERSISTENCE] Error: Failed to load FAISS index" << std::endl;
            return false;
        }

        // Проверяем размерность
        if (external_docs_index_->d != embedding_dim) {
            std::cerr << "[RAG PERSISTENCE] Error: Embedding dimension mismatch. Expected: "
                      << embedding_dim << ", got: " << external_docs_index_->d << std::endl;
            return false;
        }

        // Синхронизируем размерность генератора с загруженным индексом,
        // чтобы запросы эмбеддились в ту же размерность
        if (embedding_generator_) {
            embedding_generator_->set_embedding_dimension(static_cast<int>(external_docs_index_->d));
        }

        std::cout << "[RAG PERSISTENCE] FAISS index loaded successfully" << std::endl;

        // Загружаем чанки
        external_chunks_.clear();
        external_chunks_.reserve(chunk_count);

        const auto& chunks_json = metadata["chunks"];
        for (const auto& chunk_meta : chunks_json) {
            RagChunk chunk;
            chunk.content = chunk_meta["content"].get<std::string>();
            chunk.document_id = chunk_meta["document_id"].get<std::string>();
            chunk.chunk_index = chunk_meta["chunk_index"].get<int>();
            // Code-aware metadata (backward-compatible: fields may be absent in v1 indexes)
            if (chunk_meta.contains("content_type")) {
                chunk.content_type = static_cast<RagChunk::ContentType>(chunk_meta["content_type"].get<int>());
            }
            if (chunk_meta.contains("language")) chunk.language = chunk_meta["language"].get<std::string>();
            if (chunk_meta.contains("file_path")) chunk.file_path = chunk_meta["file_path"].get<std::string>();
            if (chunk_meta.contains("symbol_name")) chunk.symbol_name = chunk_meta["symbol_name"].get<std::string>();
            if (chunk_meta.contains("parent_symbol")) chunk.parent_symbol = chunk_meta["parent_symbol"].get<std::string>();
            if (chunk_meta.contains("start_line")) chunk.start_line = chunk_meta["start_line"].get<int>();
            if (chunk_meta.contains("end_line")) chunk.end_line = chunk_meta["end_line"].get<int>();

            external_chunks_.push_back(std::move(chunk));
        }

        std::cout << "[RAG PERSISTENCE] Chunks loaded successfully: " << external_chunks_.size() << std::endl;

        // === ИНИЦИАЛИЗАЦИЯ CHAT_HISTORY_INDEX_ ===
        // После загрузки профиля нужно убедиться, что chat_history_index_ корректно инициализирован
        int actual_dim = external_docs_index_->d;
        if (!chat_history_index_ || chat_history_chunks_.empty()) {
            chat_history_index_ = create_optimized_index(actual_dim);
            std::cout << "[RAG PERSISTENCE] chat_history_index_ initialized with dimension " << actual_dim << std::endl;
        }
        // =========================================

        std::cout << "[RAG PERSISTENCE] RAG index ready for use!" << std::endl;

        // === Проверка совместимости индекса с текущей моделью эмбеддингов ===
        // Индекс построен эмбеддингами определённой модели. Если активная модель
        // сменилась, векторы из разных моделей несопоставимы и поиск будет неверным.
        index_needs_reindex_ = false;
        reindex_reason_.clear();
        index_embedding_model_ = index_model;

        std::string active_model = embedding_model_path_.empty()
            ? "" : std::filesystem::path(embedding_model_path_).filename().string();

        // Старые индексы (version < 3) не записывали embedding_model в metadata —
        // они построены n-gram fallback и несопоставимы с серверными эмбеддингами
        // даже при совпадении размерности.
        if (metadata_version < 3) {
            index_needs_reindex_ = true;
            reindex_reason_ = "Индекс создан старой версией (без метаданных модели эмбеддингов) "
                "и построен без серверных эмбеддингов. Требуется переиндексация.";
            std::cerr << "[RAG PERSISTENCE] WARNING: " << reindex_reason_ << std::endl;
        } else if (!active_model.empty() && !index_model.empty() && active_model != index_model) {
            index_needs_reindex_ = true;
            reindex_reason_ = "Индекс построен моделью «" + index_model
                + "», а активная модель — «" + active_model
                + "». Векторы разных моделей несопоставимы. Требуется переиндексация.";
            std::cerr << "[RAG PERSISTENCE] WARNING: " << reindex_reason_ << std::endl;
        } else if (embedding_generator_ &&
                   static_cast<int>(external_docs_index_->d) != embedding_generator_->get_embedding_dimension()) {
            index_needs_reindex_ = true;
            reindex_reason_ = "Индекс имеет размерность " + std::to_string(external_docs_index_->d)
                + ", а текущая модель эмбеддингов — "
                + std::to_string(embedding_generator_->get_embedding_dimension())
                + ". Требуется переиндексация.";
            std::cerr << "[RAG PERSISTENCE] WARNING: " << reindex_reason_ << std::endl;
        }

        return true;

    } catch (const std::exception& e) {
        std::cerr << "[RAG PERSISTENCE] Error: Failed to load index: " << e.what() << std::endl;
        return false;
    }
#else
    std::cerr << "[RAG PERSISTENCE] Error: FAISS not available" << std::endl;
    return false;
#endif
}

void RagManager::clear_all_indexes() {
    std::cout << "[RAG PERSISTENCE] Clearing all indexes..." << std::endl;
    
#ifdef USE_FAISS
    external_docs_index_.reset();
    chat_history_index_.reset();
#endif
    
    external_chunks_.clear();
    chat_history_chunks_.clear();

    // Очищаем кэш эмбеддингов
    query_embedding_cache_.clear();

    std::cout << "[RAG PERSISTENCE] All indexes cleared" << std::endl;
}

// ============================================================================
// Управление профилями индексов
// ============================================================================

bool RagManager::initialize_profile_manager(const std::string& profiles_directory) {
    std::cout << "[RAG PROFILE] Initializing profile manager..." << std::endl;
    return profile_manager_.initialize(profiles_directory);
}

bool RagManager::create_index_profile(const std::string& profile_name, const std::string& source_directory) {
    std::cout << "[RAG PROFILE] Creating profile: " << profile_name << std::endl;

    if (!profile_manager_.create_profile(profile_name, source_directory)) {
        return false;
    }

    // If source directory is specified, auto-index all documents in it
    if (!source_directory.empty()) {
        std::cout << "[RAG PROFILE] Auto-indexing documents from: " << source_directory << std::endl;

        // Switch to the new profile first
        if (!profile_manager_.set_current_profile(profile_name)) {
            std::cerr << "[RAG PROFILE] Failed to switch to new profile" << std::endl;
            return false;
        }

        // Clear and reload index for new profile
        clear_all_indexes();
        initialize_indexes();

        // Process all supported files in the directory
        namespace fs = std::filesystem;
        int indexed_count = 0;
        for (const auto& entry : fs::recursive_directory_iterator(source_directory)) {
            if (!entry.is_regular_file()) continue;

            std::string ext = entry.path().extension().string();
            // Support common document formats
            if (ext == ".txt" || ext == ".md" || ext == ".json" || ext == ".csv" ||
                ext == ".log" || ext == ".py" || ext == ".js" || ext == ".cpp" ||
                ext == ".h" || ext == ".java" || ext == ".rs" || ext == ".go") {
                std::cout << "[RAG PROFILE] Indexing: " << entry.path().string() << std::endl;
                if (process_document(entry.path().string(), false)) {
                    indexed_count++;
                }
            }
        }

        std::cout << "[RAG PROFILE] Indexed " << indexed_count << " documents from " << source_directory << std::endl;

        // Save the updated index
        save_index();
    }

    return true;
}

bool RagManager::switch_index_profile(const std::string& profile_name) {
    std::cout << "[RAG PROFILE] Switching to profile: " << profile_name << std::endl;
    
    // Переключаем профиль
    if (!profile_manager_.set_current_profile(profile_name)) {
        std::cerr << "[RAG PROFILE] Failed to switch profile" << std::endl;
        return false;
    }

    // Очищаем текущий индекс
    clear_all_indexes();

    // Загружаем индекс для нового профиля
    return load_index_for_current_profile();
}

bool RagManager::delete_index_profile(const std::string& profile_name, bool delete_index_file) {
    std::cout << "[RAG PROFILE] Deleting profile: " << profile_name << std::endl;
    return profile_manager_.delete_profile(profile_name, delete_index_file);
}

std::vector<std::string> RagManager::get_index_profile_names() const {
    return profile_manager_.get_profile_names();
}

std::string RagManager::get_current_index_profile() const {
    return profile_manager_.get_current_profile();
}

std::string RagManager::get_current_index_path() const {
    return profile_manager_.get_current_index_path();
}

bool RagManager::load_index_for_current_profile() {
    std::string index_path = profile_manager_.get_current_index_path();
    std::string metadata_path = profile_manager_.get_current_metadata_path();

    if (index_path.empty() || metadata_path.empty()) {
        std::cerr << "[RAG PROFILE] Error: Current profile has no index path" << std::endl;
        return false;
    }

    std::cout << "[RAG PROFILE] Loading index for profile from: " << index_path << std::endl;
    return load_index(index_path);
}

std::string RagManager::get_current_profile_source_directory() const {
    std::string current = profile_manager_.get_current_profile();
    if (current.empty()) return "";
    const RagIndexProfile* profile = profile_manager_.get_profile(current);
    if (!profile) return "";
    return profile->source_directory;
}

bool RagManager::reindex_current_profile() {
    std::string current = profile_manager_.get_current_profile();
    if (current.empty()) {
        std::cerr << "[RAG PROFILE] Error: No current profile selected" << std::endl;
        return false;
    }

    const RagIndexProfile* profile = profile_manager_.get_profile(current);
    if (!profile) {
        std::cerr << "[RAG PROFILE] Error: Profile not found: " << current << std::endl;
        return false;
    }

    std::string source_dir = profile->source_directory;
    std::vector<std::string> documents = profile->documents;

    if (source_dir.empty() && documents.empty()) {
        std::cerr << "[RAG PROFILE] Error: Profile has no source directory or documents: " << current << std::endl;
        return false;
    }

    std::cout << "[RAG PROFILE] Reindexing profile: " << current
              << " from: " << (source_dir.empty() ? "documents list" : source_dir) << std::endl;

    // Clear existing index
    clear_all_indexes();

    // Re-initialize empty index
    initialize_indexes();

    // Process all supported files in the directory
    namespace fs = std::filesystem;
    int indexed_count = 0;

    if (!source_dir.empty()) {
        for (const auto& entry : fs::recursive_directory_iterator(source_dir)) {
            if (!entry.is_regular_file()) continue;

            std::string ext = entry.path().extension().string();
            if (ext == ".txt" || ext == ".md" || ext == ".json" || ext == ".csv" ||
                ext == ".log" || ext == ".py" || ext == ".js" || ext == ".cpp" ||
                ext == ".h" || ext == ".java" || ext == ".rs" || ext == ".go") {
                std::cout << "[RAG PROFILE] Indexing: " << entry.path().string() << std::endl;
                if (process_document(entry.path().string(), false)) {
                    indexed_count++;
                }
            }
        }
    } else {
        // Профиль без source directory, но со списком документов
        for (const auto& doc : documents) {
            std::cout << "[RAG PROFILE] Indexing: " << doc << std::endl;
            if (process_document(doc, false)) {
                indexed_count++;
            }
        }
    }

    std::cout << "[RAG PROFILE] Reindexed " << indexed_count << " documents" << std::endl;

    // Save the updated index
    save_index();

    // После успешной переиндексации флаг несовместимости сбрасывается
    reset_needs_reindex();

    return true;
}

std::vector<std::string> RagManager::get_current_profile_documents() const {
    std::string current = profile_manager_.get_current_profile();
    if (current.empty()) return {};
    const RagIndexProfile* profile = profile_manager_.get_profile(current);
    if (!profile) return {};
    return profile->documents;
}

} // namespace core
} // namespace llama_gui
