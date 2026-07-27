#pragma once

#include "rag_manager.h"
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace llama_gui {
namespace core {
namespace deep_analysis {

inline std::string format_duration(int64_t ms) {
    std::ostringstream oss;
    if (ms < 1000) {
        oss << ms << "ms";
    } else if (ms < 60000) {
        oss << std::fixed << std::setprecision(1) << (ms / 1000.0) << "s";
    } else {
        oss << std::fixed << std::setprecision(1) << (ms / 60000.0) << "min";
    }
    return oss.str();
}

inline int estimate_tokens(const std::string& text) {
    return static_cast<int>(text.size() / 4);
}

inline std::vector<std::vector<RagChunk>> create_batches(
    std::vector<RagChunk>& chunks,
    int batch_size)
{
    std::vector<std::vector<RagChunk>> batches;

    for (size_t i = 0; i < chunks.size(); i += batch_size) {
        std::vector<RagChunk> batch;
        size_t end = std::min(i + static_cast<size_t>(batch_size), chunks.size());
        batch.reserve(end - i);

        for (size_t j = i; j < end; ++j) {
            batch.push_back(chunks[j]);
        }

        batches.push_back(std::move(batch));
    }

    return batches;
}

inline std::vector<std::vector<std::string>> create_batches_for_strings(
    const std::vector<std::string>& items,
    int batch_size)
{
    std::vector<std::vector<std::string>> batches;

    for (size_t i = 0; i < items.size(); i += batch_size) {
        std::vector<std::string> batch;
        size_t end = std::min(i + static_cast<size_t>(batch_size), items.size());
        batch.reserve(end - i);

        for (size_t j = i; j < end; ++j) {
            batch.push_back(items[j]);
        }

        batches.push_back(std::move(batch));
    }

    return batches;
}

inline std::string trim_summary(const std::string& text) {
    std::string result = text;

    const std::vector<std::string> markers = {
        "\n\n=== ", "\n\n---", "\n\n**", "###", "```",
        "\n\nРезюме:", "\n\nОтвет:", "\n\nВывод:"
    };

    for (const auto& marker : markers) {
        size_t pos = result.find(marker);
        if (pos != std::string::npos) {
            result = result.substr(0, pos);
        }
    }

    size_t start = result.find_first_not_of(" \t\n\r");
    size_t end = result.find_last_not_of(" \t\n\r");

    if (start == std::string::npos) {
        return "";
    }

    return result.substr(start, end - start + 1);
}

} // namespace deep_analysis
} // namespace core
} // namespace llama_gui
