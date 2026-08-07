#include "../../include/ui/language_selector.h"
#include <iostream>

namespace llama_gui {
namespace ui {

bool LanguageSelector::renderComboBox(const std::string& label, float width) {
#ifdef USE_IMGUI
    auto& loc = getLocalizationManager();
    std::vector<LanguageInfo> languages = loc.getLanguageInfos();
    std::string current_code = loc.getCurrentLanguageCode();

    // Find current language index
    int current_index = 0;
    for (size_t i = 0; i < languages.size(); ++i) {
        if (languages[i].code == current_code) {
            current_index = static_cast<int>(i);
            break;
        }
    }

    // Set width if specified
    if (width > 0) {
        ImGui::PushItemWidth(width);
    }

    bool changed = false;
    // Convert vector<string> to vector<const char*> for ImGui::Combo
    std::vector<const char*> language_name_ptrs;
    language_name_ptrs.reserve(languages.size());
    for (const auto& lang : languages) {
        language_name_ptrs.push_back(lang.display_name.c_str());
    }

    if (ImGui::Combo(label.c_str(), &current_index, language_name_ptrs.data(), static_cast<int>(language_name_ptrs.size()))) {
        if (current_index >= 0 && current_index < static_cast<int>(languages.size())) {
            loc.setCurrentLanguage(languages[current_index].code);
            language_changed_ = true;
            changed = true;
        }
    }

    if (width > 0) {
        ImGui::PopItemWidth();
    }

    return changed;
#else
    std::cout << "Warning: ImGui not available, language selector rendering skipped" << std::endl;
    return false;
#endif
}

bool LanguageSelector::renderButtons(const std::string& label) {
#ifdef USE_IMGUI
    auto& loc = getLocalizationManager();
    std::vector<LanguageInfo> languages = loc.getLanguageInfos();
    std::string current_code = loc.getCurrentLanguageCode();

    bool changed = false;

    // Render label
    if (!label.empty()) {
        ImGui::Text("%s:", label.c_str());
        ImGui::SameLine();
    }

    // Render buttons for each language
    for (size_t i = 0; i < languages.size(); ++i) {
        bool is_current = (languages[i].code == current_code);

        if (is_current) {
            // Current language button (disabled style)
            ImGui::BeginDisabled();
        }

        if (ImGui::Button(languages[i].display_name.c_str())) {
            if (!is_current) {
                loc.setCurrentLanguage(languages[i].code);
                language_changed_ = true;
                changed = true;
            }
        }

        if (is_current) {
            ImGui::EndDisabled();
        }

        // Add spacing between buttons
        if (i < languages.size() - 1) {
            ImGui::SameLine();
            ImGui::Spacing();
            ImGui::SameLine();
        }
    }

    return changed;
#else
    std::cout << "Warning: ImGui not available, language selector rendering skipped" << std::endl;
    return false;
#endif
}

bool LanguageSelector::renderCompact(const std::string& label) {
#ifdef USE_IMGUI
    auto& loc = getLocalizationManager();
    std::vector<LanguageInfo> languages = loc.getLanguageInfos();
    std::string current_code = loc.getCurrentLanguageCode();

    // Find current language index
    int current_index = 0;
    for (size_t i = 0; i < languages.size(); ++i) {
        if (languages[i].code == current_code) {
            current_index = static_cast<int>(i);
            break;
        }
    }

    bool changed = false;

    // Render label
    if (!label.empty()) {
        ImGui::Text("%s:", label.c_str());
        ImGui::SameLine();
    }

    // Render compact combo
    if (ImGui::BeginCombo("##language_selector", languages[current_index].display_name.c_str())) {
        for (int i = 0; i < static_cast<int>(languages.size()); ++i) {
            const bool is_selected = (i == current_index);
            if (ImGui::Selectable(languages[i].display_name.c_str(), is_selected)) {
                loc.setCurrentLanguage(languages[i].code);
                language_changed_ = true;
                changed = true;
                current_index = i;
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    return changed;
#else
    std::cout << "Warning: ImGui not available, language selector rendering skipped" << std::endl;
    return false;
#endif
}

} // namespace ui
} // namespace llama_gui
