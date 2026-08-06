#include "../include/ui/settings_dialog_grammar.h"
#include "../external/imgui/imgui.h"
#include <iostream>

namespace llama_gui {
namespace ui {

GrammarDialog::GrammarDialog(Settings& settings)
    : settings_(settings) {
    temp_schema_buf_[0] = '\0';
}

GrammarDialog::~GrammarDialog() = default;

void GrammarDialog::HelpMarker(const std::string& desc) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void GrammarDialog::render() {
    auto& grammar = settings_.grammar();

    // =========================================================================
    // Grammar Section
    // =========================================================================
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Grammar");
    ImGui::Separator();

    // Grammar rules (inline)
    {
        char buf[1024];
        strncpy(buf, grammar.grammar.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        if (ImGui::InputTextMultiline("Grammar Rules##grammar", buf, sizeof(buf),
                                       ImVec2(-FLT_MIN, 100))) {
            grammar.grammar = buf;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("GBNF grammar rules (inline)");
    }

    // Grammar file
    {
        char buf[512];
        strncpy(buf, grammar.grammar_file.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        if (ImGui::InputText("Grammar File##grammar_file", buf, sizeof(buf))) {
            grammar.grammar_file = buf;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Path to GBNF grammar file");

        if (ImGui::Button("Browse##grammar_file")) {
            // TODO: Open file dialog
        }
    }

    ImGui::Separator();

    // =========================================================================
    // JSON Schema Section
    // =========================================================================
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "JSON Schema");
    ImGui::Separator();

    ImGui::Text("JSON Schema constrains output to valid JSON matching a schema.");
    ImGui::Separator();

    // JSON Schema (inline)
    {
        char buf[4096];
        strncpy(buf, grammar.json_schema.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        if (ImGui::InputTextMultiline("JSON Schema##json_schema", buf, sizeof(buf),
                                       ImVec2(-FLT_MIN, 150))) {
            grammar.json_schema = buf;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("JSON Schema (inline)");
    }

    // JSON Schema file
    {
        char buf[512];
        strncpy(buf, grammar.json_schema_file.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        if (ImGui::InputText("JSON Schema File##json_schema_file", buf, sizeof(buf))) {
            grammar.json_schema_file = buf;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Path to JSON Schema file");

        if (ImGui::Button("Browse##json_schema_file")) {
            // TODO: Open file dialog
        }
    }

    // Preset schemas
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Preset Schemas")) {
        ImGui::Indent();

        if (ImGui::Button("Simple Object")) {
            grammar.json_schema = R"({"type": "object", "properties": {"name": {"type": "string"}, "age": {"type": "number"}}})";
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Basic object with name and age");

        if (ImGui::Button("Array of Strings")) {
            grammar.json_schema = R"({"type": "array", "items": {"type": "string"}})";
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Array of strings");

        if (ImGui::Button("Question Answer")) {
            grammar.json_schema = R"({"type": "object", "properties": {"answer": {"type": "string"}, "confidence": {"type": "number"}}})";
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("QA response format");

        ImGui::Unindent();
    }

    ImGui::Separator();

    // =========================================================================
    // Chat Template Section
    // =========================================================================
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Chat Template");
    ImGui::Separator();

    // Chat template
    {
        char buf[2048];
        strncpy(buf, grammar.chat_template.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        if (ImGui::InputTextMultiline("Chat Template##chat_tpl", buf, sizeof(buf),
                                       ImVec2(-FLT_MIN, 100))) {
            grammar.chat_template = buf;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Chat template string (Jinja2 format)");
    }

    // Chat template file
    {
        char buf[512];
        strncpy(buf, grammar.chat_template_file.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        if (ImGui::InputText("Chat Template File##chat_tpl_file", buf, sizeof(buf))) {
            grammar.chat_template_file = buf;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Path to chat template file");
    }

    // Chat template kwargs
    {
        char buf[512];
        strncpy(buf, grammar.chat_template_kwargs.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        if (ImGui::InputText("Template Kwargs##chat_kwargs", buf, sizeof(buf))) {
            grammar.chat_template_kwargs = buf;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Additional kwargs for template (JSON format)");
    }

    ImGui::Separator();

    // Use Jinja
    {
        bool use_jinja = grammar.use_jinja;
        if (ImGui::Checkbox("Use Jinja##use_jinja", &use_jinja)) {
            grammar.use_jinja = use_jinja;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Enable Jinja2 template processing");
    }

    // No prefill assistant
    {
        bool no_prefill = grammar.no_prefill_assistant;
        if (ImGui::Checkbox("No Prefill Assistant##no_prefill", &no_prefill)) {
            grammar.no_prefill_assistant = no_prefill;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Don't prefill assistant message");
    }

    // System prompt file
    {
        char buf[512];
        strncpy(buf, grammar.system_prompt_file.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        if (ImGui::InputText("System Prompt File##sys_prompt", buf, sizeof(buf))) {
            grammar.system_prompt_file = buf;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Path to system prompt file");
    }

    // Default system prompt
    {
        char buf[1024];
        strncpy(buf, grammar.default_system_prompt.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        if (ImGui::InputTextMultiline("Default System Prompt##default_sys", buf, sizeof(buf),
                                       ImVec2(-FLT_MIN, 60))) {
            grammar.default_system_prompt = buf;
            modified_ = true;
        }
    }

    ImGui::Separator();

    // =========================================================================
    // Reasoning Section
    // =========================================================================
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Reasoning Format");
    ImGui::Separator();

    ImGui::Text("Reasoning format for models with chain-of-thought output.");
    ImGui::Separator();

    // Reasoning format
    ImGui::Text("Reasoning Format:");
    ImGui::SameLine();

    const char* reasoning_modes[] = { "None", "Deepseek" };
    int current_mode = (grammar.reasoning_format == llama_gui::core::GrammarSettings::ReasoningFormat::Deepseek) ? 1 : 0;

    if (ImGui::Combo("##reasoning_format", &current_mode, reasoning_modes, 2)) {
        if (current_mode == 0) {
            grammar.set_reasoning_format("none");
        } else {
            grammar.set_reasoning_format("deepseek");
        }
        modified_ = true;
    }
    ImGui::SameLine();
    HelpMarker("Format for reasoning/thinking output");

    // Reasoning budget
    {
        int budget = grammar.reasoning_budget;
        if (ImGui::SliderInt("Reasoning Budget##reasoning_budget", &budget, -1, 16384)) {
            grammar.reasoning_budget = budget;
            modified_ = true;
        }
        ImGui::SameLine();
        HelpMarker("Maximum tokens for reasoning (-1 = unlimited)");
    }

    // =========================================================================
    // Summary
    // =========================================================================
    ImGui::Separator();
    ImGui::Text("Configuration Summary:");

    ImGui::BulletText("Grammar: %s", grammar.has_grammar() ? "Configured" : "Not set");
    ImGui::BulletText("JSON Schema: %s", grammar.has_json_schema() ? "Configured" : "Not set");
    ImGui::BulletText("Chat Template: %s", grammar.has_chat_template() ? "Configured" : "Not set");
    ImGui::BulletText("System Prompt: %s", grammar.has_system_prompt() ? "Configured" : "Default");

    if (grammar.uses_reasoning()) {
        ImGui::BulletText("Reasoning: %s (budget=%d)",
                          grammar.get_reasoning_format_string().c_str(),
                          grammar.reasoning_budget);
    } else {
        ImGui::BulletText("Reasoning: Disabled");
    }
}

} // namespace ui
} // namespace llama_gui
