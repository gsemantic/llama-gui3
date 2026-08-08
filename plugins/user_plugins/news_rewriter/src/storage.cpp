#include "storage.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace news_rewriter {

namespace fs = std::filesystem;

namespace {

// Безопасный slug: буквы/цифры/дефис/подчёркивание, остальное → '_'.
std::string safe_slug(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if ((uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z') ||
            (uc >= '0' && uc <= '9') || c == '-' || c == '_') {
            out += c;
        } else {
            out += '_';
        }
    }
    if (out.empty()) out = "article";
    return out;
}

// Дата (YYYY-MM-DD) из ISO-8601 fetched_at; пусто — если не разобралось.
std::string date_of(const std::string& iso) {
    if (iso.size() >= 10) return iso.substr(0, 10);
    return "";
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return "";
    std::string out((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    return out;
}

bool write_file(const fs::path& p, const std::string& content) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    if (ec) return false;
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return out.good();
}

} // namespace

bool Storage::init(const std::string& data_dir) {
    if (data_dir.empty()) return false;
    root_ = (fs::path(data_dir) / "news_rewriter").string();
    articles_dir_ = (fs::path(root_) / "articles").string();

    std::error_code ec;
    fs::create_directories(articles_dir_, ec);
    return !ec || ec == std::errc::file_exists;
}

std::string Storage::slug_for(const std::string& url) const {
    // slug = безопасная метка + первые 12 символов sha256(url)
    const std::string hash = sha256_hex(url);
    std::string label = safe_slug(host_of(url));
    if (label.size() > 24) label.resize(24);
    return label + "_" + hash.substr(0, 12);
}

std::string Storage::article_json_path(const Article& a) const {
    return (fs::path(articles_dir_) / date_of(a.fetched_at) /
            (slug_for(a.url) + ".json")).string();
}

std::string Storage::article_md_path(const Article& a) const {
    return (fs::path(articles_dir_) / date_of(a.fetched_at) /
            (slug_for(a.url) + ".md")).string();
}

bool Storage::save_article_json(const Article& a) {
    const std::string content = article_to_json(a).dump();
    return write_file(article_json_path(a), content);
}

bool Storage::save_article_md(const Article& a) {
    std::string md;
    md += "# " + a.title_original + "\n\n";
    if (!a.title_rewritten.empty() && a.title_rewritten != a.title_original) {
        md += "### Переписанный заголовок\n\n" + a.title_rewritten + "\n\n";
    }
    md += "Источник: " + a.url + "\n";
    md += "Дата: " + a.fetched_at + "\n\n";
    md += "---\n\n";
    if (!a.body_rewritten.empty()) {
        md += "## Текст (рерайт)\n\n" + a.body_rewritten + "\n\n";
    }
    if (!a.body_original.empty()) {
        md += "## Текст (оригинал)\n\n" + a.body_original + "\n";
    }
    return write_file(article_md_path(a), md);
}

bool Storage::load_index(Json& out) const {
    const std::string raw = read_file(fs::path(root_) / "index.json");
    if (raw.empty()) {
        out = Json::object();
        return false;  // нет файла — но это не ошибка для первого запуска
    }
    bool ok = false;
    out = Json::parse(raw, &ok);
    if (!ok) out = Json::object();
    return ok;
}

bool Storage::save_index(const Json& index) {
    return write_file(fs::path(root_) / "index.json", index.dump());
}

bool Storage::load_state(Json& out) const {
    const std::string raw = read_file(fs::path(root_) / "state.json");
    if (raw.empty()) {
        out = Json::object();
        return false;
    }
    bool ok = false;
    out = Json::parse(raw, &ok);
    if (!ok) out = Json::object();
    return ok;
}

bool Storage::save_state(const Json& state) {
    return write_file(fs::path(root_) / "state.json", state.dump());
}

bool Storage::is_duplicate(const Article& a) const {
    Json index;
    if (!load_index(index) || !index.is_object()) return false;

    // По id (sha256 url) — повторный обход того же URL.
    if (index.contains(a.id)) return true;

    // По content_hash — тот же текст под новым URL.
    if (!a.content_hash.empty()) {
        for (const std::string& key : index.keys()) {
            const Json& entry = index.get(key);
            if (entry.is_object() && entry.get("content_hash").as_string() == a.content_hash) {
                return true;
            }
        }
    }
    return false;
}

void Storage::mark_written(const Article& a) {
    Json index;
    load_index(index);
    if (!index.is_object()) index = Json::object();

    Json entry = Json::object();
    entry["path"] = article_json_path(a);
    entry["content_hash"] = a.content_hash;
    index[a.id] = entry;
    save_index(index);
}

} // namespace news_rewriter
