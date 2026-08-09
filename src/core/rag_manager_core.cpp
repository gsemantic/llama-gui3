#include "../include/core/rag_manager.h"
#include "../include/core/embedding_generator.h"
#include "../include/core/embedding_proxy.h"
#include "../include/core/document_parser.h"
#include "../include/core/kv_cache_storage.h"
#include "../include/core/llama_interface.h"
#include "../include/core/llama_interface_impl.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <functional>

#ifdef USE_FAISS
#include <faiss/IndexFlat.h>
#include <faiss/IndexIVFFlat.h>
#include <faiss/index_io.h>
#endif

namespace fs = std::filesystem;

namespace llama_gui {
namespace core {

RagManager::RagManager(const std::string& embedding_model_path) {
    // Сохраняем путь модели для проверки совместимости индексов
    embedding_model_path_ = embedding_model_path;

    // Инициализируем генератор эмбеддингов
    embedding_generator_ = std::make_unique<EmbeddingGenerator>(embedding_model_path);

    // Пытаемся загрузить модель эмбеддингов
    if (embedding_generator_ && !embedding_generator_->load_model()) {
        std::cerr << "Warning: Failed to load embedding model from " << embedding_model_path << ", using fallback" << std::endl;
        embedding_generator_.reset(); // Сбрасываем, если не удалось загрузить
    }

    // Создаем FAISS индексы
    int embedding_dim = EMBEDDING_DIMENSION; // Используем размер по умолчанию или получаем из модели
    if (embedding_generator_) {
        embedding_dim = embedding_generator_->get_embedding_dimension();
    }

#ifdef USE_FAISS
    external_docs_index_ = create_optimized_index(embedding_dim);
    chat_history_index_ = create_optimized_index(embedding_dim);
#else
    external_docs_index_ = nullptr;
    chat_history_index_ = nullptr;
#endif

    // Инициализируем параметры из настроек по умолчанию
    max_chunks_in_memory_ = 1000;
    similarity_threshold_ = 0.7f;

    // Инициализируем директорию для KV-cache
    const char* home = getenv("HOME");
    if (!home) {
        home = ".";
    }
    kv_cache_directory_ = std::string(home) + "/.llama-gui/kv_cache/";

    // Создаём директорию для KV-cache
    try {
        if (!fs::exists(kv_cache_directory_)) {
            fs::create_directories(kv_cache_directory_);
        }
    } catch (const std::exception& e) {
        std::cerr << "[RAG] Warning: Failed to create KV-cache directory: " << e.what() << std::endl;
    }

    // Инициализируем llama_interface для работы с сервером
    llama_interface_ = std::make_shared<LlamaInterface>("http://localhost:8081");

    // === ПРОФИЛИ ИНДЕКСОВ: Инициализируем менеджер профилей ===
    initialize_profile_manager();

    // === ПЕРСИСТЕНТНОСТЬ: Пытаемся загрузить сохранённый индекс для текущего профиля ===
    if (load_index_for_current_profile()) {
        std::cout << "[RAG] Index loaded for current profile: " << get_current_index_profile() << std::endl;
    } else {
        std::cout << "[RAG] No index found for current profile, will use empty index" << std::endl;
    }
}

RagManager::~RagManager() {
    // Деструктор теперь автоматически освободит embedding_generator_
    // благодаря использованию std::unique_ptr
}

// ============================================================================
// KV-cache загрузка при RAG-поиске
// ============================================================================

bool RagManager::try_load_document_kv_cache(const std::vector<RagChunk>& chunks, int slot_id) {
    if (!enable_kv_cache_) {
        std::cout << "[RAG KV-CACHE] KV-cache is disabled" << std::endl;
        return false;
    }

    if (chunks.empty()) {
        std::cout << "[RAG KV-CACHE] No chunks to load KV-cache for" << std::endl;
        return false;
    }

    // Собираем уникальные document_id из чанков
    std::unordered_map<std::string, RagChunk> doc_chunks;  // doc_id → первый чанк
    for (const auto& chunk : chunks) {
        if (doc_chunks.find(chunk.document_id) == doc_chunks.end()) {
            doc_chunks[chunk.document_id] = chunk;
        }
    }

    std::cout << "[RAG KV-CACHE] Trying to load KV-cache for " << doc_chunks.size() << " document(s)" << std::endl;

    bool loaded = false;
    for (const auto& [doc_id, chunk] : doc_chunks) {
        if (has_document_kv_cache(doc_id)) {
            std::cout << "[RAG KV-CACHE] Found KV-cache for document: " << doc_id << std::endl;

            // Загружаем KV-cache в слот
            if (load_document_kv_cache(doc_id, slot_id)) {
                std::cout << "[RAG KV-CACHE] Successfully loaded KV-cache for: " << doc_id << std::endl;
                loaded = true;
            } else {
                std::cerr << "[RAG KV-CACHE] Failed to load KV-cache for: " << doc_id << std::endl;
            }
        } else {
            std::cout << "[RAG KV-CACHE] No KV-cache found for document: " << doc_id << std::endl;
        }
    }

    return loaded;
}

#ifdef USE_FAISS
std::unique_ptr<faiss::Index> RagManager::create_optimized_index(int dim) {
    // Используем IndexFlatIP для начала (не требует обучения)
    // IndexIVFFlat требует обучения на реальных данных, поэтому используем его позже
    auto index = std::make_unique<faiss::IndexFlatIP>(dim);
    return index;
}
#else
void* RagManager::create_optimized_index(int dim) {
    // Заглушка без FAISS - возвращаем nullptr
    return nullptr;
}
#endif

bool RagManager::initialize_indexes() {
    // Инициализация GGUF модели для эмбеддингов
    // Здесь будет загрузка модели all-MiniLM-L6-v2 в формате GGUF
    // Пока временная заглушка
    return true;
}

// ============================================================================
// Методы KV-cache
// ============================================================================

std::string RagManager::generate_doc_id(const std::string& file_path) const {
    // Используем hash от пути для создания уникального ID
    std::hash<std::string> hasher;
    size_t hash = hasher(file_path);

    std::stringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << hash;
    return ss.str();
}

std::string RagManager::compute_file_hash(const std::string& file_path) const {
    try {
        std::ifstream file(file_path, std::ios::binary);
        if (!file) {
            return "";
        }

        // Простой hash на основе содержимого
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();

        std::hash<std::string> hasher;
        size_t hash = hasher(content);

        std::stringstream ss;
        ss << std::hex << std::setfill('0') << std::setw(16) << hash;
        return ss.str();
    } catch (...) {
        return "";
    }
}

std::string RagManager::get_document_kv_cache_path(const std::string& doc_id) const {
    return kv_cache_directory_ + "/" + doc_id + ".bin";
}

bool RagManager::has_document_kv_cache(const std::string& doc_id) const {
    std::string path = get_document_kv_cache_path(doc_id);
    return fs::exists(path) && fs::is_regular_file(path);
}

bool RagManager::save_document_kv_cache(const std::string& doc_id, int slot_id) {
    if (!llama_interface_) {
        std::cerr << "[RAG KV-CACHE] Error: LlamaInterface not initialized" << std::endl;
        return false;
    }

    std::string cache_path = get_document_kv_cache_path(doc_id);
    std::string filename = doc_id + ".bin";

    auto result = llama_interface_->save_slot_kv_cache_detailed(slot_id, filename);

    if (result.success) {
        std::cout << "[RAG KV-CACHE] Saved KV-cache for document " << doc_id
                  << " (" << result.n_tokens << " tokens, "
                  << result.n_bytes << " bytes)" << std::endl;
        return true;
    }
    std::cerr << "[RAG KV-CACHE] Failed to save KV-cache for document " << doc_id
              << ": " << result.error_message << std::endl;
    return false;
}

bool RagManager::load_document_kv_cache(const std::string& doc_id, int slot_id) {
    if (!has_document_kv_cache(doc_id)) {
        std::cerr << "[RAG KV-CACHE] No KV-cache found for document " << doc_id << std::endl;
        return false;
    }

    if (!llama_interface_) {
        std::cerr << "[RAG KV-CACHE] Error: LlamaInterface not initialized" << std::endl;
        return false;
    }

    std::string filename = doc_id + ".bin";
    auto result = llama_interface_->restore_slot_kv_cache_detailed(slot_id, filename);

    if (result.success) {
        std::cout << "[RAG KV-CACHE] Loaded KV-cache for document " << doc_id
                  << " (" << result.n_tokens << " tokens, "
                  << result.n_bytes << " bytes, "
                  << result.processing_ms << " ms)" << std::endl;
        return true;
    } else {
        std::cerr << "[RAG KV-CACHE] Failed to load KV-cache for document " << doc_id
                  << ": " << result.error_message << std::endl;
        return false;
    }
}

bool RagManager::delete_document_kv_cache(const std::string& doc_id) {
    std::string path = get_document_kv_cache_path(doc_id);

    try {
        if (fs::exists(path) && fs::is_regular_file(path)) {
            fs::remove(path);
            std::cout << "[RAG KV-CACHE] Deleted KV-cache for document " << doc_id << std::endl;
            return true;
        }
        return false;
    } catch (const std::exception& e) {
        std::cerr << "[RAG KV-CACHE] Error deleting KV-cache: " << e.what() << std::endl;
        return false;
    }
}

int RagManager::cleanup_old_kv_caches(int older_than_seconds) {
    int deleted_count = 0;

    try {
        if (!fs::exists(kv_cache_directory_)) {
            return 0;
        }

        auto now = std::time(nullptr);

        for (const auto& entry : fs::directory_iterator(kv_cache_directory_)) {
            if (entry.is_regular_file() && entry.path().extension() == ".bin") {
                auto file_time = fs::last_write_time(entry);
                // Конвертируем file_time в time_t через подсчёт секунд с epoch
                auto file_time_t = static_cast<std::time_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        file_time.time_since_epoch()).count());
                auto file_age = std::difftime(now, file_time_t);

                if (older_than_seconds == 0 || file_age > older_than_seconds) {
                    try {
                        fs::remove(entry.path());
                        deleted_count++;
                        std::cout << "[RAG KV-CACHE] Cleaned up old cache: " << entry.path() << std::endl;
                    } catch (const std::exception& e) {
                        std::cerr << "[RAG KV-CACHE] Error deleting file " << entry.path()
                                  << ": " << e.what() << std::endl;
                    }
                }
            }
        }

        std::cout << "[RAG KV-CACHE] Cleaned up " << deleted_count << " KV-cache files" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[RAG KV-CACHE] Error during cleanup: " << e.what() << std::endl;
    }

    return deleted_count;
}

RagManager::RagDocumentInfo RagManager::get_document_info(const std::string& doc_id) const {
    RagDocumentInfo info;
    info.doc_id = doc_id;
    info.kv_cache_available = has_document_kv_cache(doc_id);
    info.kv_cache_path = get_document_kv_cache_path(doc_id);

    return info;
}

} // namespace core
} // namespace llama_gui
