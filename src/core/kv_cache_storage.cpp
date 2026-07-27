#include "../include/core/kv_cache_storage.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <functional>

namespace fs = std::filesystem;

namespace llama_gui {
namespace core {

// ============================================================================
// Private Impl Class
// ============================================================================

class KVCacheStorage::Impl {
public:
    std::string base_path;

    explicit Impl(const std::string& path) : base_path(path) {}

    /**
     * @brief Создать директорию если не существует
     */
    bool ensure_directory_exists() {
        try {
            if (!fs::exists(base_path)) {
                return fs::create_directories(base_path);
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    /**
     * @brief Генерировать имя файла из doc_id
     */
    std::string generate_filename(const std::string& doc_id) {
        // Используем hash от doc_id для имени файла
        std::hash<std::string> hasher;
        size_t hash = hasher(doc_id);

        std::stringstream ss;
        ss << std::hex << std::setfill('0') << std::setw(16) << hash;
        return ss.str() + ".bin";
    }

    /**
     * @brief Вычислить MD5 hash файла
     */
    std::string compute_file_hash(const std::string& filepath) {
        // Простая реализация hash на основе содержимого файла
        // Для production используйте настоящий MD5/SHA256
        std::ifstream file(filepath, std::ios::binary);
        if (!file) {
            return "";
        }

        std::streambuf* buf = file.rdbuf();
        std::streamsize size = file.seekg(0, std::ios::end).tellg();
        file.seekg(0, std::ios::beg);

        std::vector<char> buffer(size);
        file.read(buffer.data(), size);

        // Простой hash для демонстрации
        std::hash<std::string> hasher;
        size_t hash = hasher(std::string(buffer.begin(), buffer.end()));

        std::stringstream ss;
        ss << std::hex << std::setfill('0') << std::setw(16) << hash;
        return ss.str();
    }
};

// ============================================================================
// Public Methods
// ============================================================================

KVCacheStorage::KVCacheStorage(const std::string& base_path)
    : pImpl(std::make_unique<Impl>(base_path)), base_path_(base_path) {
    // Создаём директорию при инициализации
    pImpl->ensure_directory_exists();
}

KVCacheStorage::~KVCacheStorage() = default;

std::string KVCacheStorage::get_cache_path(const std::string& doc_id) const {
    std::string filename = pImpl->generate_filename(doc_id);
    return get_full_path(filename);
}

std::string KVCacheStorage::get_full_path(const std::string& filename) const {
    return base_path_ + "/" + filename;
}

bool KVCacheStorage::has_cache(const std::string& doc_id) const {
    std::string path = get_cache_path(doc_id);
    return fs::exists(path) && fs::is_regular_file(path);
}

bool KVCacheStorage::delete_cache(const std::string& doc_id) {
    try {
        std::string path = get_cache_path(doc_id);
        if (fs::exists(path) && fs::is_regular_file(path)) {
            fs::remove(path);
            std::cout << "[KV-CACHE] Deleted cache for document: " << doc_id << std::endl;
            return true;
        }
        return false;
    } catch (const std::exception& e) {
        std::cerr << "[KV-CACHE] Error deleting cache: " << e.what() << std::endl;
        return false;
    }
}

size_t KVCacheStorage::get_cache_size(const std::string& doc_id) const {
    try {
        std::string path = get_cache_path(doc_id);
        if (fs::exists(path) && fs::is_regular_file(path)) {
            return fs::file_size(path);
        }
        return 0;
    } catch (...) {
        return 0;
    }
}

std::string KVCacheStorage::get_cache_hash(const std::string& doc_id) const {
    std::string path = get_cache_path(doc_id);
    if (!fs::exists(path) || !fs::is_regular_file(path)) {
        return "";
    }
    return pImpl->compute_file_hash(path);
}

int KVCacheStorage::cleanup_old_caches(int older_than_seconds) {
    int deleted_count = 0;

    try {
        if (!fs::exists(base_path_)) {
            return 0;
        }

        auto now = std::time(nullptr);

        for (const auto& entry : fs::directory_iterator(base_path_)) {
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
                        std::cout << "[KV-CACHE] Cleaned up old cache: " << entry.path() << std::endl;
                    } catch (const std::exception& e) {
                        std::cerr << "[KV-CACHE] Error deleting file " << entry.path()
                                  << ": " << e.what() << std::endl;
                    }
                }
            }
        }

        std::cout << "[KV-CACHE] Cleaned up " << deleted_count << " old cache files" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[KV-CACHE] Error during cleanup: " << e.what() << std::endl;
    }

    return deleted_count;
}

int KVCacheStorage::clear_all_caches() {
    int deleted_count = 0;

    try {
        if (!fs::exists(base_path_)) {
            return 0;
        }

        for (const auto& entry : fs::directory_iterator(base_path_)) {
            if (entry.is_regular_file() && entry.path().extension() == ".bin") {
                try {
                    fs::remove(entry.path());
                    deleted_count++;
                } catch (const std::exception& e) {
                    std::cerr << "[KV-CACHE] Error deleting file " << entry.path()
                              << ": " << e.what() << std::endl;
                }
            }
        }

        std::cout << "[KV-CACHE] Cleared all " << deleted_count << " cache files" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[KV-CACHE] Error during clear all: " << e.what() << std::endl;
    }

    return deleted_count;
}

std::vector<std::string> KVCacheStorage::get_all_cached_documents() const {
    std::vector<std::string> documents;

    try {
        if (!fs::exists(base_path_)) {
            return documents;
        }

        for (const auto& entry : fs::directory_iterator(base_path_)) {
            if (entry.is_regular_file() && entry.path().extension() == ".bin") {
                // Возвращаем имя файла без расширения
                std::string filename = entry.path().stem().string();
                documents.push_back(filename);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[KV-CACHE] Error listing cached documents: " << e.what() << std::endl;
    }

    return documents;
}

size_t KVCacheStorage::get_total_storage_size() const {
    size_t total_size = 0;

    try {
        if (!fs::exists(base_path_)) {
            return 0;
        }

        for (const auto& entry : fs::directory_iterator(base_path_)) {
            if (entry.is_regular_file() && entry.path().extension() == ".bin") {
                total_size += fs::file_size(entry);
            }
        }
    } catch (...) {
        return 0;
    }

    return total_size;
}

int KVCacheStorage::get_file_count() const {
    int count = 0;

    try {
        if (!fs::exists(base_path_)) {
            return 0;
        }

        for (const auto& entry : fs::directory_iterator(base_path_)) {
            if (entry.is_regular_file() && entry.path().extension() == ".bin") {
                count++;
            }
        }
    } catch (...) {
        return 0;
    }

    return count;
}

std::string KVCacheStorage::get_base_path() const {
    return base_path_;
}

bool KVCacheStorage::is_writable() const {
    try {
        // Проверяем существование директории
        if (!fs::exists(base_path_)) {
            // Пытаемся создать
            return pImpl->ensure_directory_exists();
        }

        // Проверяем права на запись
        auto perms = fs::status(base_path_).permissions();
        return (perms & fs::perms::owner_write) != fs::perms::none;
    } catch (...) {
        return false;
    }
}

size_t KVCacheStorage::get_available_space() const {
    try {
        auto space = fs::space(base_path_);
        return space.available;
    } catch (...) {
        return 0;
    }
}

} // namespace core
} // namespace llama_gui
