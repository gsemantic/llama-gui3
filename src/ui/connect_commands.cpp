#include "ui/connect_commands.h"
#include "core/env_manager.h"
#include <sstream>
#include <utility>

namespace llama_gui {
namespace ui {

ConnectCommands::ConnectCommands() = default;

ConnectCommands::~ConnectCommands() = default;

bool ConnectCommands::initialize(core::Settings* settings) {
    if (!settings) {
        return false;
    }

    settings_ = settings;
    return true;
}

ConnectCommandResult ConnectCommands::execute(const std::string& command) {
    auto args = parse_arguments(command);

    if (args.empty()) {
        ConnectCommandResult result;
        result.success = false;
        result.message = "Empty command";
        return result;
    }

    std::string cmd = args[0];

    // Remove leading '/'
    if (!cmd.empty() && cmd[0] == '/') {
        cmd = cmd.substr(1);
    }

    // Remove the first argument (command name)
    args.erase(args.begin());

    if (cmd == "connect") {
        return handle_connect_command(args);
    }

    ConnectCommandResult result;
    result.success = false;
    result.message = "Unknown command: " + cmd;
    return result;
}

ConnectCommandResult ConnectCommands::handle_connect_command(const std::vector<std::string>& args) {
    ConnectCommandResult result;

    if (args.empty()) {
        result.success = false;
        result.message = "Usage: /connect <provider> [options]";
        return result;
    }

    std::string provider = args[0];

    if (provider == "gigachat" || provider == "giga" || provider == "GigaChat") {
        return connect_to_gigachat();
    } else if (provider == "list" || provider == "providers") {
        return list_providers();
    } else {
        result.success = false;
        result.message = "Unknown provider: " + provider + ". Use 'gigachat' or 'list'.";
        return result;
    }
}

ConnectCommandResult ConnectCommands::connect_to_gigachat() {
    ConnectCommandResult result;

    // Configure settings for GigaChat via gpt2giga proxy
    auto& cp = settings_->cloud_provider();

    // Set up GigaChat configuration
    cp.provider_name = "GigaChat";
    cp.endpoint_url = "http://localhost:8090/v1";  // gpt2giga proxy endpoint
    cp.model_id = "GigaChat-3-Ultra";
    cp.enabled = true;
    cp.timeout_ms = 120000;  // 2 minutes timeout for slower responses

    // Save settings to profile
    std::string profile = settings_->get_current_profile_name();
    if (!profile.empty()) {
        settings_->save_profile(profile);
    }

    result.success = true;
    result.message = "Successfully connected to GigaChat via gpt2giga proxy (http://localhost:8090/v1)";
    result.provider = "GigaChat";
    result.endpoint = "http://localhost:8090/v1";

    return result;
}

ConnectCommandResult ConnectCommands::list_providers() {
    ConnectCommandResult result;

    result.success = true;
    result.message = "Available providers:\n"
                    "  - gigachat: Connect to GigaChat via gpt2giga proxy\n"
                    "  - Usage: /connect gigachat";
    result.providers = {"gigachat"};

    return result;
}

std::vector<std::string> ConnectCommands::parse_arguments(const std::string& command) {
    std::vector<std::string> args;
    std::istringstream iss(command);
    std::string arg;

    while (iss >> arg) {
        args.push_back(arg);
    }

    return args;
}

void ConnectCommands::set_on_result(std::function<void(const ConnectCommandResult&)> callback) {
    on_result_ = std::move(callback);
}

std::string ConnectCommands::format_result(const ConnectCommandResult& result) const {
    std::ostringstream oss;

    if (result.success) {
        oss << "✓ ";
    } else {
        oss << "✗ ";
    }

    oss << result.message;

    return oss.str();
}

} // namespace ui
} // namespace llama_gui
