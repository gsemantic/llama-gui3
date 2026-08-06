#include "../../include/core/embedding_generator.h"
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <sstream>

#ifdef USE_CURL
#include <curl/curl.h>
#endif

namespace llama_gui {
namespace core {

EmbeddingGenerator::EmbeddingGenerator(const std::string& model_path)
    : model_path_(model_path) {}

bool EmbeddingGenerator::load_model() {
    if (!server_url_.empty()) {
        std::cout << "[EmbeddingGenerator] Server mode: " << server_url_ << std::endl;
        model_loaded_ = true;
        return true;
    }

    if (!model_path_.empty()) {
        std::cout << "[EmbeddingGenerator] Local model path: " << model_path_ << std::endl;
        model_loaded_ = true;
        return true;
    }

    std::cerr << "Warning: No embedding model path or server URL configured, using fallback" << std::endl;
    model_loaded_ = true;
    return true;
}

std::vector<float> EmbeddingGenerator::generate_embedding(const std::string& text) {
    if (!model_loaded_) {
        std::cerr << "Error: EmbeddingGenerator not loaded" << std::endl;
        return std::vector<float>(embedding_dimension_, 0.0f);
    }

    if (text.empty()) {
        return std::vector<float>(embedding_dimension_, 0.0f);
    }

    // Check cache first
    std::vector<float> cached;
    if (cache_enabled_ && get_from_cache(text, cached)) {
        return cached;
    }

    std::vector<float> result;

    // Try server-based embedding first
    if (!server_url_.empty()) {
        result = generate_via_server(text);
        if (!result.empty()) {
            if (cache_enabled_) put_to_cache(text, result);
            return result;
        }
        std::cerr << "Warning: Server embedding failed, falling back to local" << std::endl;
    }

    // Fallback to n-gram hash based embedding
    result = generate_fallback(text);
    if (cache_enabled_ && !result.empty()) {
        put_to_cache(text, result);
    }
    return result;
}

std::vector<float> EmbeddingGenerator::generate_via_server(const std::string& text) {
#ifdef USE_CURL
    // Truncate text if too long (model limit: 512 tokens ≈ 1500 chars for mixed text)
    // Use conservative limit to avoid HTTP 500 errors
    static const size_t MAX_CHARS = 1500;
    std::string truncated_text = text;
    if (truncated_text.size() > MAX_CHARS) {
        truncated_text = truncated_text.substr(0, MAX_CHARS);
        // Try to break at sentence boundary
        size_t last_period = truncated_text.rfind('.');
        size_t last_newline = truncated_text.rfind('\n');
        size_t break_pos = std::max(last_period, last_newline);
        if (break_pos > MAX_CHARS / 2) {
            truncated_text = truncated_text.substr(0, break_pos + 1);
        }
        std::cerr << "[EmbeddingGenerator] Truncated text from " << text.size() 
                  << " to " << truncated_text.size() << " chars" << std::endl;
    }

    std::string url = server_url_;
    if (url.back() != '/') url += '/';
    url += "v1/embeddings";

    // Build JSON request
    std::string json_body = "{\"input\": \"";

    // Escape JSON string
    for (char c : truncated_text) {
        switch (c) {
            case '"':  json_body += "\\\""; break;
            case '\\': json_body += "\\\\"; break;
            case '\n': json_body += "\\n"; break;
            case '\r': json_body += "\\r"; break;
            case '\t': json_body += "\\t"; break;
            default:   json_body += c; break;
        }
    }
    json_body += "\", \"model\": \"embedding\"}";

    CURL* curl = curl_easy_init();
    if (!curl) {
        return {};
    }

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            auto* resp = static_cast<std::string*>(userdata);
            resp->append(ptr, size * nmemb);
            return size * nmemb;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
        std::cerr << "[EmbeddingGenerator] Server request failed: "
                  << (res != CURLE_OK ? curl_easy_strerror(res) : "HTTP " + std::to_string(http_code))
                  << std::endl;
        return {};
    }

    // Parse response JSON to extract embedding vector
    // Response format: {"data": [{"embedding": [...], "index": 0}], ...}
    size_t embedding_start = response.find("\"embedding\":");
    if (embedding_start == std::string::npos) {
        std::cerr << "[EmbeddingGenerator] No 'embedding' field in response" << std::endl;
        return {};
    }

    size_t bracket_start = response.find('[', embedding_start);
    size_t bracket_end = response.find(']', bracket_start);
    if (bracket_start == std::string::npos || bracket_end == std::string::npos) {
        std::cerr << "[EmbeddingGenerator] Malformed embedding array" << std::endl;
        return {};
    }

    std::string values_str = response.substr(bracket_start + 1, bracket_end - bracket_start - 1);
    std::vector<float> embedding;
    std::istringstream iss(values_str);
    std::string token;

    while (std::getline(iss, token, ',')) {
        try {
            float val = std::stof(token);
            embedding.push_back(val);
        } catch (...) {
            continue;
        }
    }

    if (!embedding.empty()) {
        embedding_dimension_ = static_cast<int>(embedding.size());
        normalize_embedding(embedding);
        std::cout << "[EmbeddingGenerator] Server embedding: " << embedding.size() << " dimensions" << std::endl;
    }

    return embedding;
#else
    (void)text;
    std::cerr << "[EmbeddingGenerator] CURL not available for server embedding" << std::endl;
    return {};
#endif
}

std::vector<float> EmbeddingGenerator::generate_fallback(const std::string& text) {
    std::vector<float> embedding(embedding_dimension_, 0.0f);

    const int ngram_size = 3;
    std::string normalized_text = text;

    for (auto& c : normalized_text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    for (size_t i = 0; i < normalized_text.size(); ++i) {
        std::string ngram = normalized_text.substr(i, std::min<size_t>(ngram_size, normalized_text.size() - i));
        std::hash<std::string> hasher;
        size_t hash = hasher(ngram);

        for (int j = 0; j < embedding_dimension_; ++j) {
            float contribution = std::sin(static_cast<float>((hash >> (j % 32)) & 0xFF)) * 0.5f + 0.5f;
            embedding[j] += contribution / static_cast<float>(normalized_text.size() - ngram_size + 1);
        }
    }

    float length_factor = std::log(1.0f + static_cast<float>(text.size())) / 10.0f;
    for (int j = 0; j < embedding_dimension_; ++j) {
        embedding[j] += length_factor * std::sin(static_cast<float>(j) * 0.1f);
    }

    normalize_embedding(embedding);
    return embedding;
}

void EmbeddingGenerator::normalize_embedding(std::vector<float>& embedding) {
    float norm = 0.0f;
    for (float val : embedding) {
        norm += val * val;
    }
    norm = std::sqrt(norm);
    if (norm > 0.0f) {
        for (float& val : embedding) {
            val /= norm;
        }
    }
}

// ============================================================================
// Cache methods
// ============================================================================

bool EmbeddingGenerator::get_from_cache(const std::string& text, std::vector<float>& result) {
    std::shared_lock<std::shared_mutex> lock(cache_mutex_);
    auto it = embedding_cache_.find(text);
    if (it != embedding_cache_.end()) {
        result = it->second;
        return true;
    }
    return false;
}

void EmbeddingGenerator::put_to_cache(const std::string& text, const std::vector<float>& embedding) {
    std::unique_lock<std::shared_mutex> lock(cache_mutex_);
    
    // Evict oldest entries if cache is full
    if (embedding_cache_.size() >= max_cache_size_) {
        // Simple eviction: remove first entry (could be improved with LRU)
        embedding_cache_.erase(embedding_cache_.begin());
    }
    
    embedding_cache_[text] = embedding;
}

size_t EmbeddingGenerator::get_cache_size() const {
    std::shared_lock<std::shared_mutex> lock(cache_mutex_);
    return embedding_cache_.size();
}

void EmbeddingGenerator::clear_cache() {
    std::unique_lock<std::shared_mutex> lock(cache_mutex_);
    embedding_cache_.clear();
}

EmbeddingGenerator::~EmbeddingGenerator() {}

} // namespace core
} // namespace llama_gui
