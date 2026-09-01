#include "agents/grep_agent.h"
#include <filesystem>
#include <fstream>
#include <regex>
#include <algorithm>

namespace fs = std::filesystem;

namespace agents {

GrepAgent::GrepAgent() = default;
GrepAgent::~GrepAgent() { shutdown(); }

const char* GrepAgent::name() const { return "grep_agent"; }
const char* GrepAgent::description() const {
    return "Content search agent. Searches file contents using regex patterns.";
}
const char* GrepAgent::version() const { return "1.0.0"; }

bool GrepAgent::initialize(AgentContext* context) {
    if (!context) return false;
    context_ = context;
    initialized_ = true;
    context_->info("GrepAgent", "Initialized");
    return true;
}

void GrepAgent::shutdown() {
    initialized_ = false;
    context_ = nullptr;
}

AgentCapability GrepAgent::capabilities() const {
    return AgentCapability::FILE_READ | AgentCapability::DIRECTORY_LIST;
}

bool GrepAgent::is_ready() const { return initialized_; }

AgentResult GrepAgent::execute(const AgentRequest& request) {
    if (!initialized_) return AgentResult::error("Not initialized");

    const auto& action = request.action();
    if (action == "grep") return handle_grep(request);

    return AgentResult::error("Unknown action: " + action + ". Use 'grep'");
}

AgentResult GrepAgent::handle_grep(const AgentRequest& request) {
    auto pattern_str = request.get_param<std::string>("pattern", "");
    auto search_path = request.get_param<std::string>("path", ".");
    auto include = request.get_param<std::string>("include", "");

    if (pattern_str.empty()) return AgentResult::error("Missing 'pattern' parameter");

    // Проверка границ проекта
    if (context_ && !context_->get_project_root().empty()) {
        std::error_code ec;
        fs::path abs = fs::absolute(search_path, ec);
        if (!ec && !context_->is_within_project(abs.string())) {
            return AgentResult::error(
                "Access denied: path is outside the project.\n"
                "Path: " + search_path + "\n"
                "Project root: " + context_->get_project_root());
        }
    }

    // Validate regex
    std::regex re;
    try {
        re = std::regex(pattern_str);
    } catch (const std::regex_error& e) {
        return AgentResult::error("Invalid regex pattern: " + std::string(e.what()));
    }

    if (!fs::exists(search_path)) {
        return AgentResult::error("Path not found: " + search_path);
    }

    nlohmann::json matches = nlohmann::json::array();
    int total_matches = 0;

    auto search_entry = [&](const fs::path& file_path) {
        search_file(file_path.string(), pattern_str, matches);
        if (!include.empty()) {
            // Filter by include pattern (simple extension match)
            auto ext = file_path.extension().string();
            if (ext != include) {
                // Remove last added entries for this file
                while (!matches.empty() &&
                       matches.back()["file"].get<std::string>() == file_path.string()) {
                    matches.erase(matches.end() - 1);
                }
            }
        }
    };

    if (fs::is_regular_file(search_path)) {
        search_entry(fs::path(search_path));
    } else {
        try {
            for (const auto& entry : fs::recursive_directory_iterator(search_path)) {
                if (!entry.is_regular_file()) continue;

                // Skip binary files and common non-text dirs
                auto path_str = entry.path().string();
                if (path_str.find("/.git/") != std::string::npos) continue;
                if (path_str.find("/build/") != std::string::npos) continue;
                if (path_str.find("/node_modules/") != std::string::npos) continue;

                search_entry(entry.path());
            }
        } catch (const fs::filesystem_error&) {
            // Skip inaccessible directories
        }
    }

    return AgentResult(AgentStatus::OK,
                       "Found " + std::to_string(matches.size()) + " matches")
            .with("matches", matches)
            .with("total_matches", (int)matches.size())
            .with("pattern", pattern_str)
            .with("path", search_path)
            .with("include", include);
}

void GrepAgent::search_file(const std::string& file_path, const std::string& pattern_str,
                             nlohmann::json& results) const {
    std::ifstream file(file_path);
    if (!file.is_open()) return;

    std::regex re(pattern_str);
    std::string line;
    int line_num = 0;

    while (std::getline(file, line)) {
        ++line_num;
        if (std::regex_search(line, re)) {
            nlohmann::json match;
            match["file"] = file_path;
            match["line"] = line_num;
            match["content"] = line;
            results.push_back(match);
        }
    }
}

} // namespace agents
