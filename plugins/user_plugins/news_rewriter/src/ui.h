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
    // Колбэк сохранения конфигурации (main-поток): применяет её воркеру
    // (ReloadConfig) и сохраняет в настройки хоста.
    std::function<void(const Config&)> on_save = nullptr;
    // Колбэк закрытия окна (main-поток): скрывает окно плагина в хосте
    // (window_set_visible). Вызывается, когда пользователь нажимает крестик.
    std::function<void()> on_close = nullptr;
};

// Отрисовка окна "News Rewriter" (Dear ImGui). Вызывать из ll_plugin_render,
// когда окно видимо.
void render_news_rewriter_window(UiDeps& deps);

} // namespace news_rewriter
