#include "../include/ui/settings_dialog_logging.h"
#include "../include/ui/localization_manager.h"
#include "../external/imgui/imgui.h"
#include <iostream>

namespace llama_gui {
namespace ui {

LoggingDialog::LoggingDialog(Settings& settings)
    : settings_(settings) {}

LoggingDialog::~LoggingDialog() = default;

void LoggingDialog::HelpMarker(const std::string& desc) {
    ImGui::TextDisabled(TR("help_icon"));
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void LoggingDialog::render_output_section() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), TR("logging.output"));
    ImGui::Separator();

    auto& runtime = settings_.server_runtime();

    // Log disabled
    {
        bool disabled = runtime.log_disabled;
        if (ImGui::Checkbox(TR("logging.disable"), &disabled)) {
            runtime.log_disabled = disabled;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("logging.disable.help"));
    }

    ImGui::Separator();

    // Log file
    {
        char buf[512];
        strncpy(buf, runtime.log_file.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        if (ImGui::InputText(TR("logging.file"), buf, sizeof(buf))) {
            runtime.log_file = buf;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("logging.file.help"));

        if (ImGui::Button(TR("logging.browse"))) {
            // TODO: Open file dialog
        }
    }

    // Log format
    ImGui::Text(TR("logging.format"));
    ImGui::SameLine();

    const char* formats[] = { TR("logging.format.text"), TR("logging.format.json") };
    int current_format = (runtime.log_format == "json") ? 1 : 0;

    if (ImGui::Combo(TR("logging.format"), &current_format, formats, 2)) {
        runtime.log_format = formats[current_format];
        modified_ = true;
    }
    ImGui::SameLine();
    HelpMarker(TR("logging.format.help"));

    // Verbose logging
    {
        bool verbose = runtime.log_verbose;
        if (ImGui::Checkbox(TR("logging.verbose"), &verbose)) {
            runtime.log_verbose = verbose;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("logging.verbose.help"));
    }
}

void LoggingDialog::render_display_section() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), TR("logging.display"));
    ImGui::Separator();

    auto& runtime = settings_.server_runtime();

    // Log colors
    {
        bool colors = runtime.log_colors;
        if (ImGui::Checkbox(TR("logging.colors"), &colors)) {
            runtime.log_colors = colors;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("logging.colors.help"));
    }

    // Log prefix
    {
        bool prefix = runtime.log_prefix;
        if (ImGui::Checkbox(TR("logging.prefix"), &prefix)) {
            runtime.log_prefix = prefix;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("logging.prefix.help"));
    }

    // Log timestamps
    {
        bool timestamps = runtime.log_timestamps;
        if (ImGui::Checkbox(TR("logging.timestamps"), &timestamps)) {
            runtime.log_timestamps = timestamps;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("logging.timestamps.help"));
    }
}

void LoggingDialog::render_verbosity_section() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), TR("logging.verbosity"));
    ImGui::Separator();

    auto& runtime = settings_.server_runtime();

    // Verbosity level
    {
        int verbosity = runtime.log_verbosity;
        if (ImGui::SliderInt(TR("logging.verbosity.level"), &verbosity, 0, 3)) {
            runtime.log_verbosity = verbosity;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker(TR("logging.verbosity.help"));
    }

    ImGui::Separator();

    // Display current verbosity
    ImGui::Text(TR("logging.current_verbosity"));
    ImGui::SameLine();

    const char* verbosity_labels[] = { 
        TR("logging.verbosity.quiet"), 
        TR("logging.verbosity.errors"), 
        TR("logging.verbosity.warnings"), 
        TR("logging.verbosity.info") 
    };
    int idx = runtime.log_verbosity;
    if (idx < 0) idx = 0;
    if (idx > 3) idx = 3;

    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", verbosity_labels[idx]);

    // Quick preset buttons
    ImGui::Separator();
    if (ImGui::Button(TR("logging.preset.quiet"))) {
        runtime.log_verbosity = 0;
        runtime.log_disabled = false;
        modified_ = true;
    }
    ImGui::SameLine();
    HelpMarker(TR("logging.preset.quiet.help"));

    if (ImGui::Button(TR("logging.preset.errors"))) {
        runtime.log_verbosity = 1;
        runtime.log_disabled = false;
        modified_ = true;
    }
    ImGui::SameLine();
    HelpMarker(TR("logging.preset.errors.help"));

    if (ImGui::Button(TR("logging.preset.standard"))) {
        runtime.log_verbosity = 2;
        runtime.log_disabled = false;
        runtime.log_prefix = true;
        runtime.log_timestamps = true;
        modified_ = true;
    }
    ImGui::SameLine();
    HelpMarker(TR("logging.preset.standard.help"));

    if (ImGui::Button(TR("logging.preset.verbose"))) {
        runtime.log_verbosity = 3;
        runtime.log_verbose = true;
        runtime.log_disabled = false;
        modified_ = true;
    }
    ImGui::SameLine();
    HelpMarker(TR("logging.preset.verbose.help"));
}

void LoggingDialog::render() {
    // =========================================================================
    // Output Section
    // =========================================================================
    render_output_section();

    ImGui::Separator();

    // =========================================================================
    // Display Section
    // =========================================================================
    render_display_section();

    ImGui::Separator();

    // =========================================================================
    // Verbosity Section
    // =========================================================================
    render_verbosity_section();

    // =========================================================================
    // Summary
    // =========================================================================
    ImGui::Separator();
    ImGui::Text(TR("logging.summary"));

    auto& runtime = settings_.server_runtime();

    if (runtime.log_disabled) {
        ImGui::BulletText(TR("logging.disabled"));
    } else {
        ImGui::BulletText(TR("logging.enabled"));
        ImGui::BulletText("  %s: %s", TR("logging.file"), runtime.log_file.empty() ? TR("logging.stdout") : runtime.log_file.c_str());
        ImGui::BulletText("  %s: %s", TR("logging.format"), runtime.log_format.c_str());
        ImGui::BulletText("  %s: %d", TR("logging.verbosity.level"), runtime.log_verbosity);
        ImGui::BulletText("  %s: %s%s%s", TR("logging.options"),
                          runtime.log_colors ? TR("logging.colors_opt") : "",
                          runtime.log_prefix ? TR("logging.prefix_opt") : "",
                          runtime.log_timestamps ? TR("logging.timestamps_opt") : "");
        if (runtime.log_verbose) {
            ImGui::BulletText(TR("logging.verbose_mode"));
        }
    }
}

} // namespace ui
} // namespace llama_gui
