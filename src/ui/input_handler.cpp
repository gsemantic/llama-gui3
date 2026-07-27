#include "../include/ui/input_handler.h"
#include <iostream>
#include <unordered_map>

namespace llama_gui {
namespace ui {

InputHandler::InputHandler()
    : has_focus_(true)
{
}

void InputHandler::initialize() {
    std::cout << "InputHandler initialized" << std::endl;
}

#ifdef USE_SDL2
bool InputHandler::handleSDLEvent(const SDL_Event& event) {
    switch (event.type) {
        case SDL_KEYDOWN:
            handleKeyDown(event.key.keysym.sym, static_cast<SDL_Keymod>(event.key.keysym.mod));
            return true;

        case SDL_KEYUP:
            handleKeyUp(event.key.keysym.sym, static_cast<SDL_Keymod>(event.key.keysym.mod));
            return true;

        case SDL_MOUSEBUTTONDOWN:
            handleMouseButtonDown(event.button.button, event.button.x, event.button.y);
            return true;

        case SDL_MOUSEBUTTONUP:
            handleMouseButtonUp(event.button.button, event.button.x, event.button.y);
            return true;

        case SDL_MOUSEMOTION:
            handleMouseMotion(event.motion.x, event.motion.y,
                            event.motion.xrel, event.motion.yrel);
            return true;

        case SDL_MOUSEWHEEL:
            handleMouseWheel(event.wheel.x, event.wheel.y);
            return true;

        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ||
                event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                bool has_focus = (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED);
                if (window_focus_changed_callback_) {
                    window_focus_changed_callback_(has_focus);
                }
                has_focus_ = has_focus;
            }
            return true;

        default:
            return false;
    }
}

void InputHandler::handleSDLEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        handleSDLEvent(event);
    }
}
#endif

void InputHandler::handleKeyDown(SDL_Keycode keycode, SDL_Keymod modifiers) {
    if (key_down_callback_) {
        key_down_callback_(keycode, modifiers);
    }

    // Check for shortcuts
    handleShortcut(keycode, modifiers);
}

void InputHandler::handleKeyUp(SDL_Keycode keycode, SDL_Keymod modifiers) {
    if (key_up_callback_) {
        key_up_callback_(keycode, modifiers);
    }
}

void InputHandler::handleMouseButtonDown(Uint8 button, int x, int y) {
    if (mouse_button_down_callback_) {
        mouse_button_down_callback_(button, x, y);
    }
}

void InputHandler::handleMouseButtonUp(Uint8 button, int x, int y) {
    if (mouse_button_down_callback_) {
        mouse_button_down_callback_(button, x, y);
    }
}

void InputHandler::handleMouseMotion(int x, int y, int relative_x, int relative_y) {
    if (mouse_motion_callback_) {
        mouse_motion_callback_(x, y, relative_x, relative_y);
    }
}

void InputHandler::handleMouseWheel(int x, int y) {
    if (mouse_wheel_callback_) {
        mouse_wheel_callback_(x, y);
    }
}

void InputHandler::handleWindowFocusChanged(bool has_focus) {
    if (window_focus_changed_callback_) {
        window_focus_changed_callback_(has_focus);
    }
    has_focus_ = has_focus;
}

void InputHandler::registerShortcut(const std::string& name, SDL_Keycode key,
                                    SDL_Keymod modifiers, std::function<void()> callback) {
    Shortcut shortcut;
    shortcut.name = name;
    shortcut.key = key;
    shortcut.modifiers = modifiers;
    shortcut.callback = callback;
    shortcuts_[name] = shortcut;
}

void InputHandler::registerWindowToggleShortcut(const std::string& window_name,
                                               SDL_Keycode key, SDL_Keymod modifiers) {
    window_toggle_shortcuts_[window_name] = key;
}

void InputHandler::handleShortcut(SDL_Keycode key, SDL_Keymod modifiers) {
    // Mask out irrelevant modifier flags (Num Lock, Caps Lock, etc.)
    // Only keep the modifiers we care about: Ctrl, Shift, Alt, GUI
    const SDL_Keymod relevant_mods = static_cast<SDL_Keymod>(
        KMOD_CTRL | KMOD_SHIFT | KMOD_ALT | KMOD_GUI);
    SDL_Keymod filtered_mods = static_cast<SDL_Keymod>(modifiers & relevant_mods);

    // Check for window toggle shortcuts
    for (const auto& [window_name, shortcut_key] : window_toggle_shortcuts_) {
        if (key == shortcut_key && filtered_mods == KMOD_NONE) {
            if (shortcut_callback_) {
                shortcut_callback_(window_name);
            }
            return;
        }
    }

    // Check for registered shortcuts
    for (const auto& [name, shortcut] : shortcuts_) {
        if (key == shortcut.key && filtered_mods == shortcut.modifiers) {
            if (shortcut.callback) {
                shortcut.callback();
            }
            return;
        }
    }
}

} // namespace ui
} // namespace llama_gui
