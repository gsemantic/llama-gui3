#include "headless_browser_panel.h"

#include <imgui.h>
#include <headless_browser/headless_browser.h>

#include "chat_interface.h"

namespace llama_gui {
namespace ui {

HeadlessBrowserPanel::HeadlessBrowserPanel() {
    std::strncpy(browser_path_, "chromium", sizeof(browser_path_) - 1);
    std::strncpy(screenshot_path_, "headless_screenshot.png",
                 sizeof(screenshot_path_) - 1);
}

HeadlessBrowserPanel::~HeadlessBrowserPanel() = default;

void HeadlessBrowserPanel::do_render() {
    std::string url = url_;
    if (url.empty()) {
        status_ = "Введите URL для рендеринга.";
        return;
    }

    headless_browser::RenderOptions opts;
    opts.browser_path = browser_path_;
    if (user_agent_[0] != '\0') opts.user_agent = user_agent_;
    opts.timeout_ms = timeout_ms_;
    opts.virtual_time_budget_ms = virtual_time_budget_ms_;

    if (!headless_browser::available(opts)) {
        error_ = "Браузер не найден: " + opts.browser_path +
                 ". Установите chromium и укажите путь.";
        status_ = error_;
        have_result_ = false;
        return;
    }

    std::string err;
    std::string dom = headless_browser::render_dom(url, opts, &err);
    if (dom.empty()) {
        error_ = "Ошибка рендеринга: " + err;
        status_ = error_;
        have_result_ = false;
        return;
    }

    result_ = dom;
    result_buf_.assign(dom.begin(), dom.end());
    result_buf_.push_back('\0');
    have_result_ = true;
    status_ = "DOM получен (" + std::to_string(dom.size()) + " байт).";
}

void HeadlessBrowserPanel::do_screenshot() {
    std::string url = url_;
    if (url.empty()) {
        status_ = "Введите URL для скриншота.";
        return;
    }

    headless_browser::RenderOptions opts;
    opts.browser_path = browser_path_;
    if (user_agent_[0] != '\0') opts.user_agent = user_agent_;
    opts.timeout_ms = timeout_ms_;

    if (!headless_browser::available(opts)) {
        error_ = "Браузер не найден: " + opts.browser_path;
        status_ = error_;
        return;
    }

    std::string err;
    if (headless_browser::screenshot(url, screenshot_path_, opts, &err)) {
        status_ = "Скриншот сохранён: " + std::string(screenshot_path_);
    } else {
        error_ = "Ошибка скриншота: " + err;
        status_ = error_;
    }
}

void HeadlessBrowserPanel::render(bool* p_open) {
    if (!ImGui::Begin("Headless-браузер", p_open)) {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped(
        "Рендеринг веб-страниц через headless-браузер (Chromium). Это НЕ "
        "серверный режим приложения, а отдельный браузер для получения DOM/скриншота.");
    ImGui::Separator();

    ImGui::InputText("URL", url_, sizeof(url_));
    ImGui::InputText("Путь к браузеру", browser_path_, sizeof(browser_path_));
    ImGui::InputText("User-Agent (пусто = по умолчанию)", user_agent_,
                     sizeof(user_agent_));
    ImGui::InputInt("Таймаут, мс", &timeout_ms_);
    ImGui::InputInt("Virtual time budget, мс", &virtual_time_budget_ms_);

    if (ImGui::Button("Рендерить DOM")) do_render();
    ImGui::SameLine();
    if (ImGui::Button("Скриншот")) do_screenshot();

    ImGui::InputText("Путь скриншота", screenshot_path_, sizeof(screenshot_path_));

    if (!status_.empty()) {
        ImGui::TextWrapped("%s", status_.c_str());
    }
    ImGui::Separator();

    // Область вывода: read-only multiline с переносом строк и поддержкой
    // выделения текста мышью и копирования. Конфигурация максимально
    // приближена к чат-вводу (chat_interface_input.cpp), где InputTextMultiline
    // уже корректно работает с мышью в этом приложении: те же флаги
    // (WordWrap, NoHorizontalScroll, CallbackAlways, AllowTabInput) + ReadOnly.
    // Встроенное меню InputText (правый клик → Copy) копирует выделенный
    // фрагмент, плюс работает Ctrl+C.
    if (have_result_ && !result_buf_.empty()) {
        // Явная ширина (как в чат-вводе): InputTextMultiline с width=0 в этом
        // контексте не растягивается и рендерится крошечным, из-за чего в него
        // нельзя кликнуть/выделить текст.
        const float avail_w = ImGui::GetContentRegionAvail().x;
        ImGui::InputTextMultiline(
            "##hb_result",
            result_buf_.data(),
            result_buf_.size(),
            ImVec2(avail_w, -30),
            ImGuiInputTextFlags_ReadOnly |
            ImGuiInputTextFlags_NoHorizontalScroll |
            ImGuiInputTextFlags_WordWrap |
            ImGuiInputTextFlags_AllowTabInput |
            ImGuiInputTextFlags_CallbackAlways,
            [](ImGuiInputTextCallbackData*) { return 0; });
    } else {
        ImGui::BeginChild("##hb_result_empty", ImVec2(0, -30), true);
        ImGui::TextDisabled("Здесь появится отрендеренный DOM страницы.");
        ImGui::EndChild();
    }

    if (ImGui::Button("Отправить в чат") && have_result_ && chat_) {
        chat_->set_input_text(result_);
        chat_->focus_input();
    }
    ImGui::SameLine();
    if (ImGui::Button("Копировать") && have_result_) {
        ImGui::SetClipboardText(result_.c_str());
    }

    ImGui::End();
}

} // namespace ui
} // namespace llama_gui
