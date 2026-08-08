#pragma once

#include <string>

#include "common.h"
#include "json.h"

namespace news_rewriter {

// Хранилище на диске: каталог статей + index.json + state.json.
// Не зависит от хоста — работает с указанной корневой директорией.
//
// Структура:
//   <root>/news_rewriter/articles/<YYYY-MM-DD>/<slug>.json
//   <root>/news_rewriter/articles/<YYYY-MM-DD>/<slug>.md
//   <root>/news_rewriter/index.json   — { "<id>": {"path":..., "content_hash":...} }
//   <root>/news_rewriter/state.json   — { "last_run": "...", ... }
class Storage {
public:
    // Создаёт структуру каталогов. root = <data_dir>/news_rewriter/.
    bool init(const std::string& data_dir);

    // Создаёт структуру каталогов в произвольном корне (пользовательская
    // выходная папка). root используется напрямую, без суффикса news_rewriter.
    bool init_root(const std::string& root);

    const std::string& root() const { return root_; }
    bool ready() const { return !root_.empty(); }

    // Уникальное имя файла для статьи (slug).
    std::string slug_for(const std::string& url) const;
    std::string article_json_path(const Article& a) const;
    std::string article_md_path(const Article& a) const;

    // Сохранение статьи (json = метаданные+текст, md = человекочитаемый рерайт).
    bool save_article_json(const Article& a);
    bool save_article_md(const Article& a);

    // index.json — url/ид → файл (для дедупликации).
    bool load_index(Json& out) const;
    bool save_index(const Json& index);

    // state.json — прогресс/статусы/счётчики.
    bool load_state(Json& out) const;
    bool save_state(const Json& state);

    // Дедупликация: по id (sha256(url)) или content_hash.
    bool is_duplicate(const Article& a) const;
    // Заносит статью в index.json (id → путь + content_hash).
    void mark_written(const Article& a);

private:
    std::string root_;        // <data_dir>/news_rewriter/
    std::string articles_dir_;  // <root>/articles
};

} // namespace news_rewriter
