#pragma once

#include <functional>

#include "config.h"

namespace news_rewriter {

class Worker;

// Зависимости UI: рабочий поток (источник команд/состояния) и колбэк
// сохранения настроек. Все методы render вызываются ТОЛЬКО из main-потока
// (ll_plugin_render).
struct UiDeps {
    Worker* worker = nullptr;
    // Каталог данных приложения (для пути к .env с секретами плагина).
    std::string data_dir;
    // Колбэк сохранения конфигурации (main-поток): применяет её воркеру
    // (ReloadConfig) и сохраняет в настройки хоста.
    std::function<void(const Config&)> on_save = nullptr;
    // Колбэк закрытия окна (main-поток): скрывает окно плагина в хосте
    // (window_set_visible). Вызывается, когда пользователь нажимает крестик.
    std::function<void()> on_close = nullptr;

    // Профили настроек плагина (автономные файлы в data_dir/news_rewriter/profiles).
    std::string active_profile;                                       // имя активного профиля
    std::function<std::vector<std::string>()> list_profiles;          // список имён профилей
    std::function<Config(const std::string&)> profile_load;            // выбрать профиль → конфиг + активировать
    std::function<void(const std::string&, const Config&)> profile_save;  // создать/перезаписать + активировать
    std::function<void(const std::string&)> profile_delete;           // удалить профиль
    std::function<std::string()> profile_active;                      // имя активного профиля (текущее)
};

// Отрисовка окна "News Rewriter" (Dear ImGui). Вызывать из ll_plugin_render,
// когда окно видимо.
void render_news_rewriter_window(UiDeps& deps);

} // namespace news_rewriter
