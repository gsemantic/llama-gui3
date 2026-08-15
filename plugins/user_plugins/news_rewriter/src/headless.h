#pragma once

#include <string>

#include "config.h"

namespace news_rewriter {

// Рендеринг страниц через headless-браузер (Chromium). Сайты, отдающие контент
// через JS (SPA, напр. VK.ru), при обычном HTTP-фетче возвращают пустую
// «оболочку», из которой невозможно извлечь статью. Headless-браузер
// исполняет JS и сериализует уже отрендеренный DOM (опция --dump-dom).
class HeadlessRenderer {
public:
    // Доступен ли браузер (исполняемый файл найден) для данной конфигурации.
    bool available(const NetworkConfig& cfg) const;

    // Рендерит URL и возвращает сериализованный DOM в UTF-8. При сбое или
    // отсутствии браузера возвращает пустую строку и (опционально) текст ошибки.
    std::string render(const std::string& url, const NetworkConfig& cfg,
                       std::string* error = nullptr);

    // Истина, если HTML похож на пустую JS-оболочку (почти нет видимого текста).
    // Используется для авто-фолбэка: обычный фетч вернул «оболочку» → пробуем
    // headless-рендер.
    bool is_thin_content(const std::string& html) const;

private:
    // Разрешает browser_path в абсолютный путь к исполняемому файлу (поиск в PATH
    // для имени без слэшей). Возвращает false, если файл недоступен.
    bool resolve_browser(const NetworkConfig& cfg, std::string& out_path) const;
};

} // namespace news_rewriter
