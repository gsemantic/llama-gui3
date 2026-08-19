#pragma once

#include <string>

namespace headless_browser {

// Настройки запуска headless-браузера (Chromium). Одно и то же АПИ используется
// и основным приложением, и плагинами — чтобы не дублировать код рендеринга.
//
// ВАЖНО: это «headless-браузер» (рендеринг веб-страниц через Chromium). Не
// путать с «серверным режимом (без GUI)» самого приложения llama-gui
// (флаг --headless в src/main.cpp) — это совсем другая функция.
struct RenderOptions {
    std::string user_agent;                 // пусто = дефолтный User-Agent Chromium
    std::string browser_path = "chromium"; // исполняемый файл (имя в PATH или путь)
    int timeout_ms = 30000;                 // предельное время работы браузера (wall-clock)
    int virtual_time_budget_ms = 15000;     // бюджет виртуального времени для JS
    int window_width = 1280;                // ширина окна (для скриншота)
    int window_height = 2400;               // высота окна (для скриншота)
    int max_output_bytes = 32 * 1024 * 1024;// защита от переполнения вывода рендера
};

// Доступен ли браузер (исполняемый файл найден в PATH/пути).
bool available(const RenderOptions& opts);

// Рендерит страницу в headless-браузере и возвращает сериализованный DOM в
// UTF-8 (опция Chromium --dump-dom). Пустая строка + error при сбое.
// Вывод ограничен opts.max_output_bytes (лишнее отсекается, процесс убивается).
std::string render_dom(const std::string& url, const RenderOptions& opts,
                        std::string* error = nullptr);

// Делает скриншот страницы (PNG) по пути out_path. true при успехе.
bool screenshot(const std::string& url, const std::string& out_path,
                const RenderOptions& opts, std::string* error = nullptr);

// Число «видимых» букв в HTML: без содержимого <script>/<style> и самих тегов.
// Латиница + любые не-ASCII байты (в UTF-8 — кириллица/прочее). Используется,
// чтобы отличить пустую JS-оболочку (единицы букв) от реальной страницы.
std::size_t visible_letter_count(const std::string& html);

// Истина, если HTML похож на пустую JS-оболочку (почти нет видимого текста) —
// признак SPA (напр. VK.ru), которому нужен headless-рендер. По умолчанию
// порог 200 видимых букв; оценка намеренно грубая.
bool is_thin_content(const std::string& html, std::size_t threshold = 200);

} // namespace headless_browser
