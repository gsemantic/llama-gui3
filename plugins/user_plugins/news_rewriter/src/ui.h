#pragma once

namespace news_rewriter {

class Worker;

// Зависимости UI: рабочий поток (источник команд/состояния).
// Все методы render вызываются ТОЛЬКО из main-потока (ll_plugin_render).
struct UiDeps {
    Worker* worker = nullptr;
};

// Отрисовка окна "News Rewriter" (Dear ImGui). Вызывать из ll_plugin_render,
// когда окно видимо.
void render_news_rewriter_window(UiDeps& deps);

} // namespace news_rewriter
