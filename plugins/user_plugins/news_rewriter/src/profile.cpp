#include "profile.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include "config.h"
#include "json.h"

namespace news_rewriter {

namespace fs = std::filesystem;

namespace {

std::string read_text_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool write_text_file(const fs::path& p, const std::string& content) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    if (ec) return false;
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return out.good();
}

} // namespace

std::string profile_slug(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if ((uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z') ||
            (uc >= '0' && uc <= '9') || c == '-' || c == '_') {
            out += c;
        } else {
            out += '_';
        }
    }
    if (out.empty()) out = "default";
    return out;
}

std::string profiles_dir(const std::string& data_dir) {
    if (data_dir.empty()) return "";
    return (fs::path(data_dir) / "news_rewriter" / "profiles").string();
}

std::string active_profile_name(const std::string& data_dir) {
    const std::string dir = profiles_dir(data_dir);
    if (dir.empty()) return "";
    const std::string raw = read_text_file(fs::path(dir) / "active.json");
    if (raw.empty()) return "";
    bool ok = false;
    const Json j = Json::parse(raw, &ok);
    if (!ok) return "";
    return j.get("active").as_string();
}

void set_active_profile(const std::string& data_dir, const std::string& name) {
    const std::string dir = profiles_dir(data_dir);
    if (dir.empty()) return;
    Json j = Json::object();
    j["active"] = name;
    write_text_file(fs::path(dir) / "active.json", j.dump());
}

std::vector<std::string> list_profiles(const std::string& data_dir) {
    std::vector<std::string> out;
    const std::string dir = profiles_dir(data_dir);
    if (dir.empty()) return out;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return out;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string fname = entry.path().filename().string();
        if (fname == "active.json") continue;
        if (fname.size() < 6 ||
            fname.compare(fname.size() - 5, 5, ".json") != 0) {
            continue;
        }
        const std::string raw = read_text_file(entry.path());
        if (raw.empty()) continue;
        bool ok = false;
        const Json j = Json::parse(raw, &ok);
        if (!ok) continue;
        const std::string nm = j.get("name").as_string();
        if (!nm.empty()) out.push_back(nm);
    }
    std::sort(out.begin(), out.end());
    return out;
}

Config load_profile(const std::string& data_dir, const std::string& name) {
    const std::string dir = profiles_dir(data_dir);
    if (dir.empty()) return default_config();
    const std::string path =
        (fs::path(dir) / (profile_slug(name) + ".json")).string();
    const std::string raw = read_text_file(path);
    if (raw.empty()) return default_config();
    bool ok = false;
    const Json j = Json::parse(raw, &ok);
    if (!ok) return default_config();
    const Json& cfg = j.get("config");
    if (!cfg.is_object()) return default_config();
    return config_from_json(cfg);
}

bool save_profile(const std::string& data_dir, const std::string& name,
                  const Config& cfg) {
    const std::string dir = profiles_dir(data_dir);
    if (dir.empty()) return false;
    Json j = Json::object();
    j["name"] = name;
    j["config"] = config_to_json(cfg);
    const bool written = write_text_file(
        fs::path(dir) / (profile_slug(name) + ".json"), j.dump());
    if (written) set_active_profile(data_dir, name);
    return written;
}

bool delete_profile(const std::string& data_dir, const std::string& name) {
    const std::string dir = profiles_dir(data_dir);
    if (dir.empty()) return false;
    const std::vector<std::string> list = list_profiles(data_dir);
    // Запрещаем удаление последнего профиля.
    if (list.size() <= 1) return false;
    const fs::path path = fs::path(dir) / (profile_slug(name) + ".json");
    std::error_code ec;
    if (!fs::exists(path, ec)) return false;
    fs::remove(path, ec);
    if (ec) return false;
    // Если удалили активный — активируем первый оставшийся.
    if (active_profile_name(data_dir) != name) return true;
    const std::vector<std::string> remaining = list_profiles(data_dir);
    if (!remaining.empty()) set_active_profile(data_dir, remaining[0]);
    return true;
}

} // namespace news_rewriter
