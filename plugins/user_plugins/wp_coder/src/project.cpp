#include "wp_coder.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

/* Запуск внешней команды, возврат stdout как строки (упрощённо, до 64КБ). */
bool run_capture(const std::string& cmd, std::string& out) {
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return false;
    char buf[4096];
    out.clear();
    while (fgets(buf, sizeof(buf), f)) out += buf;
    int rc = pclose(f);
    return rc == 0;
}

} // namespace

void project_load_settings() {
    g_state.project_dir      = setting_get_str("wp_coder.project_dir", "");
    g_state.php_bin          = setting_get_str("wp_coder.php_bin", "");
    g_state.wp_site_url      = setting_get_str("wp_coder.site_url", "");
    g_state.wp_app_user      = setting_get_str("wp_coder.app_user", "");
    g_state.wp_app_password  = setting_get_str("wp_coder.app_password", "");
    g_state.deploy_proto     = setting_get_str("wp_coder.deploy_proto", "rsync");
    g_state.deploy_host      = setting_get_str("wp_coder.deploy_host", "");
    g_state.deploy_user      = setting_get_str("wp_coder.deploy_user", "");
    g_state.deploy_pass      = setting_get_str("wp_coder.deploy_pass", "");
    g_state.deploy_port      = setting_get_str("wp_coder.deploy_port", "");
    g_state.deploy_remote_dir= setting_get_str("wp_coder.deploy_remote_dir", "");
    g_state.wp_local_url    = setting_get_str("wp_coder.local_url", "");
}

void project_save_settings() {
    setting_set_str("wp_coder.project_dir", g_state.project_dir);
    setting_set_str("wp_coder.php_bin", g_state.php_bin);
    setting_set_str("wp_coder.site_url", g_state.wp_site_url);
    setting_set_str("wp_coder.app_user", g_state.wp_app_user);
    setting_set_str("wp_coder.app_password", g_state.wp_app_password);
    setting_set_str("wp_coder.deploy_proto", g_state.deploy_proto);
    setting_set_str("wp_coder.deploy_host", g_state.deploy_host);
    setting_set_str("wp_coder.deploy_user", g_state.deploy_user);
    setting_set_str("wp_coder.deploy_pass", g_state.deploy_pass);
    setting_set_str("wp_coder.deploy_port", g_state.deploy_port);
    setting_set_str("wp_coder.deploy_remote_dir", g_state.deploy_remote_dir);
    setting_set_str("wp_coder.local_url", g_state.wp_local_url);
}

void skills_load() {
    g_state.skills.clear();
    g_state.active_skills.clear();
    std::vector<std::string> dirs;
    if (g_api && g_host) {
        const char* d = g_api->path_data_dir(g_host);
        if (d) dirs.push_back(std::string(d) + "/wp_coder/skills");
    }
    /* Каталог плагина (рядом с .so) — запасной и для поставляемых навыков. */
    dirs.push_back(std::string(WP_CODER_SKILLS_DIR));

    for (const auto& dir : dirs) {
        std::error_code ec;
        if (!fs::exists(dir, ec)) continue;
        for (auto it = fs::directory_iterator(dir, ec); it != fs::directory_iterator(); ++it) {
            if (!it->is_regular_file() || it->path().extension() != ".md") continue;
            std::ifstream f(it->path());
            std::stringstream ss; ss << f.rdbuf();
            std::string text = ss.str();
            Skill sk;
            sk.name = it->path().stem().string();
            /* Первая строка "# Имя" -> имя; вторая (после ": ") -> описание; остальное -> body. */
            std::istringstream is(text);
            std::string line;
            bool first = true;
            std::stringstream body;
            while (std::getline(is, line)) {
                if (first && !line.empty() && line[0] == '#') {
                    sk.name = line.substr(1);
                    while (!sk.name.empty() && (sk.name[0] == ' ' || sk.name[0] == '\t'))
                        sk.name.erase(0, 1);
                    first = false;
                    continue;
                }
                if (first) { first = false; }
                if (sk.description.empty() && !line.empty()) {
                    sk.description = line;
                    size_t c = sk.description.find(": ");
                    if (c != std::string::npos) sk.description = sk.description.substr(c + 2);
                } else {
                    body << line << "\n";
                }
            }
            sk.body = body.str();
            g_state.skills.push_back(std::move(sk));
        }
    }
}

void project_detect_php() {
    if (!g_state.php_bin.empty()) return;
    std::string out;
    if (run_capture("php -v", out) && !out.empty()) {
        g_state.php_bin = "php";
        project_save_settings();
    }
}

std::string project_resolve(const std::string& rel) {
    if (g_state.project_dir.empty()) return rel;
    if (rel.empty()) return g_state.project_dir;
    /* Если уже абсолютный — оставляем как есть. */
    if (rel.size() > 0 && (rel[0] == '/' || rel.find(":") == 1)) return rel;
    std::string p = g_state.project_dir;
    if (!p.empty() && p.back() != '/') p += '/';
    p += rel;
    return p;
}
