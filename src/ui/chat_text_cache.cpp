#include "../include/ui/chat_interface.h"
#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <algorithm>

namespace llama_gui {
namespace ui {

std::string ChatInterface::get_cached_formatted_text(const std::string& conversation_id, size_t message_index, const std::string& content) {
    // OPTIMIZATION: Use shared_lock for read operations, upgrade to unique_lock only for write
    std::shared_lock<std::shared_mutex> read_lock(cache_mutex_);

    // Compute content hash
    size_t content_hash = compute_content_hash(content);

    // Get or create cache entry for this conversation
    auto it = message_cache_.find(conversation_id);
    if (it == message_cache_.end()) {
        // Need to create - release read lock and acquire write lock
        read_lock.unlock();
        std::unique_lock<std::shared_mutex> write_lock(cache_mutex_);
        
        // Double-check after acquiring write lock
        auto& conversation_cache = message_cache_[conversation_id];
        conversation_cache.resize(message_index + 1);
        
        auto& cached_msg = conversation_cache[message_index];
        cached_msg.original_content = content;
        cached_msg.content_hash = content_hash;
        cached_msg.last_used = std::chrono::steady_clock::now();
        
        // Perform text wrapping
        const size_t max_line_length = 180;
        std::string* pooled_string = string_pool_.acquire(content.size() + (content.size() / max_line_length) * 2);
        std::string& wrapped_content = *pooled_string;
        wrapped_content.clear();
        cached_msg.formatted_content_pooled = pooled_string;

        size_t pos = 0;
        while (pos < content.length()) {
            size_t next_pos = pos + max_line_length;
            if (next_pos >= content.length()) {
                wrapped_content += content.substr(pos);
                break;
            }

            size_t break_pos = next_pos;
            while (break_pos > pos && content[break_pos] != ' ' && content[break_pos] != '\n') {
                break_pos--;
            }

            if (break_pos <= pos) {
                break_pos = next_pos;
            }

            wrapped_content += content.substr(pos, break_pos - pos) + "\n";
            pos = break_pos;

            while (pos < content.length() && (content[pos] == ' ' || content[pos] == '\n')) {
                pos++;
            }
        }

        return wrapped_content;
    }

    auto& conversation_cache = it->second;

    // Ensure cache vector is large enough
    if (conversation_cache.size() <= message_index) {
        read_lock.unlock();
        std::unique_lock<std::shared_mutex> write_lock(cache_mutex_);
        
        if (conversation_cache.size() <= message_index) {
            conversation_cache.resize(message_index + 1);
        }
        
        auto& cached_msg = conversation_cache[message_index];
        cached_msg.original_content = content;
        cached_msg.content_hash = content_hash;
        cached_msg.last_used = std::chrono::steady_clock::now();
        
        const size_t max_line_length = 180;
        std::string* pooled_string = string_pool_.acquire(content.size() + (content.size() / max_line_length) * 2);
        std::string& wrapped_content = *pooled_string;
        wrapped_content.clear();
        cached_msg.formatted_content_pooled = pooled_string;

        size_t pos = 0;
        while (pos < content.length()) {
            size_t next_pos = pos + max_line_length;
            if (next_pos >= content.length()) {
                wrapped_content += content.substr(pos);
                break;
            }

            size_t break_pos = next_pos;
            while (break_pos > pos && content[break_pos] != ' ' && content[break_pos] != '\n') {
                break_pos--;
            }

            if (break_pos <= pos) {
                break_pos = next_pos;
            }

            wrapped_content += content.substr(pos, break_pos - pos) + "\n";
            pos = break_pos;

            while (pos < content.length() && (content[pos] == ' ' || content[pos] == '\n')) {
                pos++;
            }
        }

        return wrapped_content;
    }

    // Check if we have a valid cached entry
    auto& cached_msg = conversation_cache[message_index];
    if (cached_msg.content_hash == content_hash &&
        cached_msg.original_content == content &&
        cached_msg.formatted_content_pooled != nullptr &&
        !cached_msg.formatted_content_pooled->empty()) {

        // Update last used time
        cached_msg.last_used = std::chrono::steady_clock::now();
        return *cached_msg.formatted_content_pooled;
    }

    // Cache miss - need to write
    read_lock.unlock();
    std::unique_lock<std::shared_mutex> write_lock(cache_mutex_);
    
    // Re-check after acquiring write lock
    auto& conversation_cache_write = message_cache_[conversation_id];
    if (conversation_cache_write.size() <= message_index) {
        conversation_cache_write.resize(message_index + 1);
    }
    
    auto& cached_msg_write = conversation_cache_write[message_index];
    cached_msg_write.original_content = content;
    cached_msg_write.content_hash = content_hash;
    cached_msg_write.last_used = std::chrono::steady_clock::now();

    const size_t max_line_length = 180;
    std::string* pooled_string = string_pool_.acquire(content.size() + (content.size() / max_line_length) * 2);
    std::string& wrapped_content = *pooled_string;
    wrapped_content.clear();
    cached_msg_write.formatted_content_pooled = pooled_string;

    size_t pos = 0;
    while (pos < content.length()) {
        size_t next_pos = pos + max_line_length;
        if (next_pos >= content.length()) {
            wrapped_content += content.substr(pos);
            break;
        }

        size_t break_pos = next_pos;
        while (break_pos > pos && content[break_pos] != ' ' && content[break_pos] != '\n') {
            break_pos--;
        }

        if (break_pos <= pos) {
            break_pos = next_pos;
        }

        wrapped_content += content.substr(pos, break_pos - pos) + "\n";
        pos = break_pos;

        while (pos < content.length() && (content[pos] == ' ' || content[pos] == '\n')) {
            pos++;
        }
    }

    return wrapped_content;
}

void ChatInterface::invalidate_cache_for_conversation(const std::string& conversation_id) {
    std::unique_lock<std::shared_mutex> lock(cache_mutex_);
    message_cache_.erase(conversation_id);
}

void ChatInterface::cleanup_old_cache_entries() {
    std::unique_lock<std::shared_mutex> lock(cache_mutex_);

    auto now = std::chrono::steady_clock::now();
    auto max_age = std::chrono::minutes(30); // Keep cache for 30 minutes

    for (auto it = message_cache_.begin(); it != message_cache_.end(); ) {
        auto& messages = it->second;
        bool has_recent_entries = false;

        for (const auto& msg : messages) {
            if (now - msg.last_used < max_age) {
                has_recent_entries = true;
                break;
            }
        }

        if (!has_recent_entries) {
            // Release pooled strings for this conversation
            for (auto& msg : messages) {
                if (msg.formatted_content_pooled) {
                    string_pool_.release(msg.formatted_content_pooled);
                    msg.formatted_content_pooled = nullptr;
                }
            }
            it = message_cache_.erase(it);
        } else {
            ++it;
        }
    }

    // Cleanup string pool
    string_pool_.cleanup();
}

size_t ChatInterface::compute_content_hash(const std::string& content) {
    // Simple FNV-1a hash for content comparison
    size_t hash = 14695981039346656037ULL; // FNV offset basis
    for (char c : content) {
        hash ^= static_cast<size_t>(c);
        hash *= 1099511628211ULL; // FNV prime
    }
    return hash;
}

} // namespace ui
} // namespace llama_gui
