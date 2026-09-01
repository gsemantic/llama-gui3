#include "agents/edit_agent.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace agents {

EditAgent::EditAgent() = default;
EditAgent::~EditAgent() { shutdown(); }

const char* EditAgent::name() const { return "edit_agent"; }
const char* EditAgent::description() const {
    return "File text replacement agent. Performs exact string replacements in files.";
}
const char* EditAgent::version() const { return "1.0.0"; }

bool EditAgent::initialize(AgentContext* context) {
    if (!context) return false;
    context_ = context;
    initialized_ = true;
    context_->info("EditAgent", "Initialized");
    return true;
}

void EditAgent::shutdown() {
    initialized_ = false;
    context_ = nullptr;
}

AgentCapability EditAgent::capabilities() const {
    return AgentCapability::FILE_READ | AgentCapability::FILE_WRITE;
}

bool EditAgent::is_ready() const { return initialized_; }

AgentResult EditAgent::execute(const AgentRequest& request) {
    if (!initialized_) return AgentResult::error("Not initialized");

    const auto& action = request.action();
    if (action == "edit") return handle_edit(request);
    if (action == "replace") return handle_replace(request);

    return AgentResult::error("Unknown action: " + action + ". Use 'edit' or 'replace'");
}

AgentResult EditAgent::handle_edit(const AgentRequest& request) {
    auto file_path = request.get_param<std::string>("file_path", "");
    auto old_text = request.get_param<std::string>("old_text", "");
    auto new_text = request.get_param<std::string>("new_text", "");

    if (file_path.empty()) return AgentResult::error("Missing 'file_path' parameter");
    if (old_text.empty()) return AgentResult::error("Missing 'old_text' parameter");

    // Проверка границ проекта
    if (context_ && !context_->get_project_root().empty()) {
        std::error_code ec;
        fs::path abs = fs::absolute(file_path, ec);
        if (!ec && !context_->is_within_project(abs.string())) {
            return AgentResult::error(
                "Access denied: file is outside the project directory.\n"
                "Path: " + file_path + "\n"
                "Project root: " + context_->get_project_root());
        }
    }

    // Read file
    std::ifstream in(file_path);
    if (!in.is_open()) return AgentResult::error("Cannot open file: " + file_path);
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    in.close();

    // Count occurrences
    size_t count = 0;
    size_t pos = 0;
    while ((pos = content.find(old_text, pos)) != std::string::npos) {
        ++count;
        pos += old_text.length();
    }

    if (count == 0) {
        return AgentResult::error("old_text not found in " + file_path);
    }
    if (count > 1) {
        return AgentResult(AgentStatus::ERROR,
            "old_text found " + std::to_string(count) +
            " times. Must be unique. Use 'replace' action or provide more context.")
            .with("matches", count);
    }

    // Replace first occurrence
    pos = content.find(old_text);
    content.replace(pos, old_text.length(), new_text);

    // Write file
    std::ofstream out(file_path);
    if (!out.is_open()) return AgentResult::error("Cannot write file: " + file_path);
    out << content;
    out.close();

    return AgentResult(AgentStatus::OK, "Replaced 1 occurrence in " + file_path)
            .with("file_path", file_path)
            .with("replacements", 1)
            .with("success", true);
}

AgentResult EditAgent::handle_replace(const AgentRequest& request) {
    auto file_path = request.get_param<std::string>("file_path", "");
    auto old_text = request.get_param<std::string>("old_text", "");
    auto new_text = request.get_param<std::string>("new_text", "");

    if (file_path.empty()) return AgentResult::error("Missing 'file_path' parameter");
    if (old_text.empty()) return AgentResult::error("Missing 'old_text' parameter");

    // Read file
    std::ifstream in(file_path);
    if (!in.is_open()) return AgentResult::error("Cannot open file: " + file_path);
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    in.close();

    // Replace all occurrences
    size_t count = 0;
    size_t pos = 0;
    while ((pos = content.find(old_text, pos)) != std::string::npos) {
        content.replace(pos, old_text.length(), new_text);
        pos += new_text.length();
        ++count;
    }

    if (count == 0) {
        return AgentResult::error("old_text not found in " + file_path);
    }

    // Write file
    std::ofstream out(file_path);
    if (!out.is_open()) return AgentResult::error("Cannot write file: " + file_path);
    out << content;
    out.close();

    return AgentResult(AgentStatus::OK,
                       "Replaced " + std::to_string(count) + " occurrences in " + file_path)
            .with("file_path", file_path)
            .with("replacements", count)
            .with("success", true);
}

} // namespace agents
