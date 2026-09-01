#include "agents/glob_agent.h"
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

namespace agents {

GlobAgent::GlobAgent() = default;
GlobAgent::~GlobAgent() { shutdown(); }

const char* GlobAgent::name() const { return "glob_agent"; }
const char* GlobAgent::description() const {
    return "File pattern matching agent. Finds files by glob patterns like src/**/*.cpp";
}
const char* GlobAgent::version() const { return "1.0.0"; }

bool GlobAgent::initialize(AgentContext* context) {
    if (!context) return false;
    context_ = context;
    initialized_ = true;
    context_->info("GlobAgent", "Initialized");
    return true;
}

void GlobAgent::shutdown() {
    initialized_ = false;
    context_ = nullptr;
}

AgentCapability GlobAgent::capabilities() const {
    return AgentCapability::FILE_READ | AgentCapability::DIRECTORY_LIST;
}

bool GlobAgent::is_ready() const { return initialized_; }

AgentResult GlobAgent::execute(const AgentRequest& request) {
    if (!initialized_) return AgentResult::error("Not initialized");

    const auto& action = request.action();
    if (action == "glob") return handle_glob(request);
    if (action == "list") return handle_list(request);

    return AgentResult::error("Unknown action: " + action + ". Use 'glob' or 'list'");
}

AgentResult GlobAgent::handle_glob(const AgentRequest& request) {
    auto pattern = request.get_param<std::string>("pattern", "");
    auto root_dir = request.get_param<std::string>("path", ".");

    if (pattern.empty()) return AgentResult::error("Missing 'pattern' parameter");

    // Проверка границ проекта
    if (context_ && !context_->get_project_root().empty()) {
        std::error_code ec;
        fs::path abs = fs::absolute(root_dir, ec);
        if (!ec && !context_->is_within_project(abs.string())) {
            return AgentResult::error(
                "Access denied: directory is outside the project.\n"
                "Path: " + root_dir + "\n"
                "Project root: " + context_->get_project_root());
        }
    }

    // Split pattern into base_dir and filename pattern
    // e.g. "src/**/*.cpp" -> base="src", pattern="**/*.cpp"
    std::string base_dir = root_dir;
    std::string file_pattern = pattern;

    // Handle ** prefix (recursive)
    bool recursive = false;
    auto star_star = pattern.find("**");
    if (star_star != std::string::npos) {
        recursive = true;
        // Extract base dir from before **
        auto prefix = pattern.substr(0, star_star);
        // Remove trailing /
        while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
        if (!prefix.empty()) base_dir = prefix;

        // Extract suffix after **
        auto suffix_pos = star_star + 2;
        while (suffix_pos < pattern.size() && pattern[suffix_pos] == '/') ++suffix_pos;
        file_pattern = pattern.substr(suffix_pos);
    }

    if (!fs::exists(base_dir) || !fs::is_directory(base_dir)) {
        return AgentResult::error("Directory not found: " + base_dir);
    }

    std::vector<std::string> results;
    walk_directory(base_dir, file_pattern, results, recursive);

    return AgentResult(AgentStatus::OK,
                       "Found " + std::to_string(results.size()) + " files")
            .with("files", results)
            .with("count", (int)results.size())
            .with("pattern", pattern)
            .with("root", base_dir);
}

AgentResult GlobAgent::handle_list(const AgentRequest& request) {
    auto dir_path = request.get_param<std::string>("path", ".");
    bool recursive = request.get_param<bool>("recursive", false);

    if (!fs::exists(dir_path)) return AgentResult::error("Directory not found: " + dir_path);
    if (!fs::is_directory(dir_path)) return AgentResult::error("Not a directory: " + dir_path);

    std::vector<std::string> results;

    if (recursive) {
        for (const auto& entry : fs::recursive_directory_iterator(dir_path)) {
            results.push_back(entry.path().string());
        }
    } else {
        for (const auto& entry : fs::directory_iterator(dir_path)) {
            results.push_back(entry.path().string());
        }
    }

    std::sort(results.begin(), results.end());

    return AgentResult(AgentStatus::OK,
                       "Found " + std::to_string(results.size()) + " entries")
            .with("files", results)
            .with("count", (int)results.size())
            .with("path", dir_path)
            .with("recursive", recursive);
}

void GlobAgent::walk_directory(const std::string& root, const std::string& pattern,
                                std::vector<std::string>& results, bool recursive) const {
    try {
        if (recursive) {
            for (const auto& entry : fs::recursive_directory_iterator(root)) {
                if (!entry.is_regular_file()) continue;
                auto filename = entry.path().filename().string();
                if (match_glob(filename, pattern)) {
                    results.push_back(entry.path().string());
                }
            }
        } else {
            for (const auto& entry : fs::directory_iterator(root)) {
                if (!entry.is_regular_file()) continue;
                auto filename = entry.path().filename().string();
                if (match_glob(filename, pattern)) {
                    results.push_back(entry.path().string());
                }
            }
        }
    } catch (const fs::filesystem_error&) {
        // Skip inaccessible directories
    }
}

bool GlobAgent::match_glob(const std::string& text, const std::string& pattern) const {
    size_t ti = 0, pi = 0;
    size_t star_pi = std::string::npos, star_ti = 0;

    while (ti < text.size()) {
        if (pi < pattern.size() && (pattern[pi] == '?' || pattern[pi] == text[ti])) {
            ++ti; ++pi;
        } else if (pi < pattern.size() && pattern[pi] == '*') {
            star_pi = pi++;
            star_ti = ti;
        } else if (star_pi != std::string::npos) {
            pi = star_pi + 1;
            ti = ++star_ti;
        } else {
            return false;
        }
    }

    while (pi < pattern.size() && pattern[pi] == '*') ++pi;
    return pi == pattern.size();
}

} // namespace agents
