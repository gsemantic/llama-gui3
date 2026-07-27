#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>

namespace llama_gui {
namespace core {

class EmbeddingGenerator {
public:
    EmbeddingGenerator(const std::string& model_path = "");
    ~EmbeddingGenerator();

    bool load_model();
    std::vector<float> generate_embedding(const std::string& text);
    bool is_loaded() const { return model_loaded_; }
    int get_embedding_dimension() const { return embedding_dimension_; }

    // Server-based embedding via llama.cpp /v1/embeddings endpoint
    void set_server_url(const std::string& url) { server_url_ = url; }
    std::string get_server_url() const { return server_url_; }
    bool has_server() const { return !server_url_.empty(); }

    // Cache management
    void set_cache_enabled(bool enabled) { cache_enabled_ = enabled; }
    bool is_cache_enabled() const { return cache_enabled_; }
    size_t get_cache_size() const;
    void clear_cache();
    void set_max_cache_size(size_t max_size) { max_cache_size_ = max_size; }

private:
    std::string model_path_;
    std::string server_url_;
    bool model_loaded_ = false;

    // Параметры модели
    int embedding_dimension_ = 384;
    int max_sequence_length_ = 512;

    // Cache for embeddings
    bool cache_enabled_ = true;
    size_t max_cache_size_ = 1000;  // Максимум 1000 кэшированных эмбеддингов
    mutable std::shared_mutex cache_mutex_;
    std::unordered_map<std::string, std::vector<float>> embedding_cache_;

    // Server-based embedding generation
    std::vector<float> generate_via_server(const std::string& text);

    // Fallback: n-gram hash based embedding
    std::vector<float> generate_fallback(const std::string& text);

    void normalize_embedding(std::vector<float>& embedding);

    // Cache helpers
    bool get_from_cache(const std::string& text, std::vector<float>& result);
    void put_to_cache(const std::string& text, const std::vector<float>& embedding);
};

} // namespace core
} // namespace llama_gui