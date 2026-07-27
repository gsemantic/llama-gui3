#include "../../include/ui/language_selector.h"
#include <iostream>

namespace llama_gui {
namespace ui {

bool LanguageSelector::renderComboBox(const std::string& label, float width) {
#ifdef USE_IMGUI
    // Callback устанавливается в main_window_init_constructor.cpp
    // Не перезаписываем его здесь

    Language current_lang = getCurrentLanguage();
    std::vector<std::string> language_names = getLocalizationManager().getAvailableLanguageNames();
    std::vector<Language> languages = getLocalizationManager().getAvailableLanguages();

    // Find current language index
    int current_index = 0;
    for (size_t i = 0; i < languages.size(); ++i) {
        if (languages[i] == current_lang) {
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
    language_name_ptrs.reserve(language_names.size());
    for (const auto& name : language_names) {
        language_name_ptrs.push_back(name.c_str());
    }
    
    if (ImGui::Combo(label.c_str(), &current_index, language_name_ptrs.data(), static_cast<int>(language_name_ptrs.size()))) {
        if (current_index >= 0 && current_index < static_cast<int>(languages.size())) {
            getLocalizationManager().setCurrentLanguage(languages[current_index]);
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
    // Callback устанавливается в main_window_init_constructor.cpp
    // Не перезаписываем его здесь

    Language current_lang = getCurrentLanguage();
    std::vector<std::string> language_names = getLocalizationManager().getAvailableLanguageNames();
    std::vector<Language> languages = getLocalizationManager().getAvailableLanguages();

    bool changed = false;

    // Render label
    if (!label.empty()) {
        ImGui::Text("%s:", label.c_str());
        ImGui::SameLine();
    }

    // Render buttons for each language
    for (size_t i = 0; i < languages.size(); ++i) {
        bool is_current = (languages[i] == current_lang);
        
        if (is_current) {
            // Current language button (disabled style)
            ImGui::BeginDisabled();
        }

        if (ImGui::Button(language_names[i].c_str())) {
            if (!is_current) {
                getLocalizationManager().setCurrentLanguage(languages[i]);
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
    // Callback устанавливается в main_window_init_constructor.cpp
    // Не перезаписываем его здесь

    Language current_lang = getCurrentLanguage();
    std::vector<std::string> language_names = getLocalizationManager().getAvailableLanguageNames();
    std::vector<Language> languages = getLocalizationManager().getAvailableLanguages();

    // Find current language index
    int current_index = 0;
    for (size_t i = 0; i < languages.size(); ++i) {
        if (languages[i] == current_lang) {
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
    if (ImGui::BeginCombo("##language_selector", language_names[current_index].c_str())) {
        for (int i = 0; i < static_cast<int>(language_names.size()); ++i) {
            const bool is_selected = (i == current_index);
            if (ImGui::Selectable(language_names[i].c_str(), is_selected)) {
                getLocalizationManager().setCurrentLanguage(languages[i]);
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