#include "../include/ui/system_prompt_settings.h"
#include "../include/core/settings.h"
#include "../external/imgui/imgui.h"
#include <iostream>
#include <cstring>

namespace llama_gui {
namespace ui {

SystemPromptSettings::SystemPromptSettings(llama_gui::core::Settings& settings)
    : settings_(settings) {
    
    // Initialize the buffer with the current system prompt
    std::string current_prompt = settings_.chat().default_system_prompt;
    size_t copy_len = std::min(current_prompt.length(), sizeof(system_prompt_buffer_) - 1);
    memcpy(system_prompt_buffer_, current_prompt.c_str(), copy_len);
    system_prompt_buffer_[copy_len] = '\0';
}

SystemPromptSettings::~SystemPromptSettings() = default;

void SystemPromptSettings::render() {
    ImGui::Text("System Prompt Settings");
    ImGui::Separator();
    
    // Display helper text
    ImGui::TextWrapped("System prompt is used to guide the AI model's behavior. It will be included at the beginning of each conversation.");
    ImGui::Spacing();
    
    // Text input for system prompt
    ImGui::Text("Default System Prompt:");
    ImGui::InputTextMultiline("##system_prompt", 
                             system_prompt_buffer_, 
                             sizeof(system_prompt_buffer_),
                             ImVec2(-1, 150),
                             ImGuiInputTextFlags_AllowTabInput);
    
    // Apply button to update settings
    if (ImGui::Button("Apply System Prompt")) {
        apply_settings();
    }
    
    ImGui::SameLine();
    
    // Reset button to restore default
    if (ImGui::Button("Reset to Default")) {
        reset_to_default();
    }
}

void SystemPromptSettings::apply_settings() {
    // Update the settings with the new system prompt
    settings_.chat().default_system_prompt = std::string(system_prompt_buffer_);
    std::cout << "System prompt updated to: " << system_prompt_buffer_ << std::endl;
}

void SystemPromptSettings::reset_to_default() {
    // Reset to default system prompt
    settings_.chat().default_system_prompt = "You are a helpful assistant.";
    
    // Update the buffer as well
    const char* default_prompt = "You are a helpful assistant.";
    size_t copy_len = std::min(strlen(default_prompt), sizeof(system_prompt_buffer_) - 1);
    memcpy(system_prompt_buffer_, default_prompt, copy_len);
    system_prompt_buffer_[copy_len] = '\0';
    
    std::cout << "System prompt reset to default" << std::endl;
}

void SystemPromptSettings::backup_current_settings() {
    // Store the current system prompt in the backup
    backup_system_prompt_ = settings_.chat().default_system_prompt;
}

void SystemPromptSettings::restore_backup() {
    // Restore the system prompt from backup
    settings_.chat().default_system_prompt = backup_system_prompt_;
    
    // Update the buffer as well
    size_t copy_len = std::min(backup_system_prompt_.length(), sizeof(system_prompt_buffer_) - 1);
    memcpy(system_prompt_buffer_, backup_system_prompt_.c_str(), copy_len);
    system_prompt_buffer_[copy_len] = '\0';
}

} // namespace ui
} // namespace llama_gui