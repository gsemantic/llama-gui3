#pragma once

#include <string>
#include <vector>

namespace llama_gui {
namespace ui {

class ChatInterface;

/**
 * @brief Панель headless-браузера (Chromium) в основном приложении.
 *
 * Не путать с «серверным режимом (без GUI)» приложения (флаг --headless в
 * src/main.cpp): здесь «headless» относится к браузеру, который рендерит
 * веб-страницы без отображения окна. Позволяет по URL получить отрендеренный
 * DOM (опция --dump-dom) либо скриншот и отправить результат в чат (например,
 * для разработки сайтов совместно с AI-кодером).
 */
class HeadlessBrowserPanel {
public:
    HeadlessBrowserPanel();
    ~HeadlessBrowserPanel();

    void render(bool* p_open);
    void set_chat_interface(ChatInterface* chat) { chat_ = chat; }

private:
    void do_render();
    void do_screenshot();

    ChatInterface* chat_ = nullptr;

    char url_[2048] = {0};
    char browser_path_[512] = {0};
    char user_agent_[512] = {0};
    char screenshot_path_[1024] = {0};
    int timeout_ms_ = 30000;
    int virtual_time_budget_ms_ = 15000;
    bool force_render_ = false;

    std::string result_;
    std::vector<char> result_buf_;  // Буфер для вывода с поддержкой выделения/копирования
    std::string error_;
    std::string status_;
    bool have_result_ = false;
};

} // namespace ui
} // namespace llama_gui
