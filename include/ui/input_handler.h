#pragma once

#include <string>
#include <functional>
#include <unordered_map>

#ifdef USE_SDL2
#include <SDL2/SDL.h>
#endif

#ifdef USE_IMGUI
#include "../external/imgui/imgui.h"
#endif

namespace llama_gui {
namespace ui {

/**
 * @brief Обработчик ввода и событий
 * 
 * Отвечает за:
 * - Обработку SDL2 событий
 * - Обработку клавиатурных сокращений (keyboard shortcuts)
 * - Обработку событий мыши
 * - Обработку событий окон
 */
class InputHandler {
public:
    InputHandler();
    ~InputHandler() = default;

    // =========================================================================
    // Инициализация
    // =========================================================================

    /**
     * @brief Инициализация обработчика
     */
    void initialize();

    // =========================================================================
    // Обработка событий
    // =========================================================================

#ifdef USE_SDL2
    /**
     * @brief Обработать SDL2 событие
     */
    bool handleSDLEvent(const SDL_Event& event);

    /**
     * @brief Обработать все SDL2 события
     */
    void handleSDLEvents();
#endif

    /**
     * @brief Обработать нажатие клавиши
     */
    void handleKeyDown(SDL_Keycode keycode, SDL_Keymod modifiers);

    /**
     * @brief Обработать отпускание клавиши
     */
    void handleKeyUp(SDL_Keycode keycode, SDL_Keymod modifiers);

    /**
     * @brief Обработать нажатие кнопки мыши
     */
    void handleMouseButtonDown(Uint8 button, int x, int y);

    /**
     * @brief Обработать отпускание кнопки мыши
     */
    void handleMouseButtonUp(Uint8 button, int x, int y);

    /**
     * @brief Обработать движение мыши
     */
    void handleMouseMotion(int x, int y, int relative_x, int relative_y);

    /**
     * @brief Обработать прокрутку колеса мыши
     */
    void handleMouseWheel(int x, int y);

    /**
     * @brief Обработать изменение фокуса окна
     */
    void handleWindowFocusChanged(bool has_focus);

    // =========================================================================
    // Горячие клавиши (keyboard shortcuts)
    // =========================================================================

    /**
     * @brief Зарегистрировать горячую клавишу
     */
    void registerShortcut(const std::string& name, SDL_Keycode key, SDL_Keymod modifiers,
                         std::function<void()> callback);

    /**
     * @brief Зарегистрировать горячую клавишу для переключения окна
     */
    void registerWindowToggleShortcut(const std::string& window_name, SDL_Keycode key,
                                      SDL_Keymod modifiers);

    /**
     * @brief Обработать горячую клавишу
     */
    void handleShortcut(SDL_Keycode key, SDL_Keymod modifiers);

    // =========================================================================
    // Callbacks
    // =========================================================================

    using KeyDownCallback = std::function<void(SDL_Keycode, SDL_Keymod)>;
    using KeyUpCallback = std::function<void(SDL_Keycode, SDL_Keymod)>;
    using ShortcutCallback = std::function<void(const std::string&)>;
    using MouseButtonDownCallback = std::function<void(Uint8, int, int)>;
    using MouseMotionCallback = std::function<void(int, int, int, int)>;
    using MouseWheelCallback = std::function<void(int, int)>;

    void setKeyDownCallback(KeyDownCallback callback) {
        key_down_callback_ = callback;
    }

    void setKeyUpCallback(KeyUpCallback callback) {
        key_up_callback_ = callback;
    }

    void setShortcutCallback(ShortcutCallback callback) {
        shortcut_callback_ = callback;
    }

    void setMouseButtonDownCallback(MouseButtonDownCallback callback) {
        mouse_button_down_callback_ = callback;
    }

    void setMouseMotionCallback(MouseMotionCallback callback) {
        mouse_motion_callback_ = callback;
    }

    void setMouseWheelCallback(MouseWheelCallback callback) {
        mouse_wheel_callback_ = callback;
    }

    void setWindowFocusChangedCallback(std::function<void(bool)> callback) {
        window_focus_changed_callback_ = callback;
    }

private:
    // Shortcut registration
    struct Shortcut {
        std::string name;
        SDL_Keycode key;
        SDL_Keymod modifiers;
        std::function<void()> callback;
    };

    std::unordered_map<std::string, Shortcut> shortcuts_;
    std::unordered_map<std::string, SDL_Keycode> window_toggle_shortcuts_;

    // Callbacks
    KeyDownCallback key_down_callback_;
    KeyUpCallback key_up_callback_;
    ShortcutCallback shortcut_callback_;
    MouseButtonDownCallback mouse_button_down_callback_;
    MouseMotionCallback mouse_motion_callback_;
    MouseWheelCallback mouse_wheel_callback_;
    std::function<void(bool)> window_focus_changed_callback_;

    // State
    bool has_focus_ = true;
};

} // namespace ui
} // namespace llama_gui
