#pragma once

#include "localization_manager.h"
#include <string>
#include <vector>

#ifdef USE_IMGUI
#include "../external/imgui/imgui.h"
#endif

namespace llama_gui {
namespace ui {

/**
 * Language Selector Component
 * Provides a UI element for switching between available languages
 */
class LanguageSelector {
public:
    LanguageSelector() = default;
    ~LanguageSelector() = default;

    // Render language selector as a combo box
    bool renderComboBox(const std::string& label = "Language", float width = 200.0f);

    // Render language selector as buttons
    bool renderButtons(const std::string& label = "Language");

    // Render compact language selector (dropdown in menu bar style)
    bool renderCompact(const std::string& label = "Lang");

    // Get current selected language
    Language getCurrentLanguage() const { 
        return getLocalizationManager().getCurrentLanguage(); 
    }

    // Check if language changed (call this after rendering)
    bool languageChanged() const { return language_changed_; }

    // Reset language changed flag
    void resetLanguageChanged() { language_changed_ = false; }

private:
    bool language_changed_ = false;
};

} // namespace ui
} // namespace llama_gui