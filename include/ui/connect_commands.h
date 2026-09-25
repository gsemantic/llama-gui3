#pragma once

#include <string>
#include <vector>
#include <functional>
#include "../../include/core/settings.h"
#include <nlohmann/json.hpp>

namespace llama_gui {
namespace ui {

struct ConnectCommandResult {
    bool success = false;
    std::string message;
    std::string provider;
    std::string endpoint;
    std::vector<std::string> providers;
    nlohmann::json data;
};

class ConnectCommands {
public:
    ConnectCommands();
    ~ConnectCommands();

    bool initialize(core::Settings* settings);
    ConnectCommandResult execute(const std::string& command);

    void set_on_result(std::function<void(const ConnectCommandResult&)> callback);
    std::string format_result(const ConnectCommandResult& result) const;

private:
    core::Settings* settings_ = nullptr;
    std::function<void(const ConnectCommandResult&)> on_result_;

    ConnectCommandResult handle_connect_command(const std::vector<std::string>& args);
    ConnectCommandResult connect_to_gigachat();
    ConnectCommandResult list_providers();
    std::vector<std::string> parse_arguments(const std::string& command);
};

} // namespace ui
} // namespace llama_gui
