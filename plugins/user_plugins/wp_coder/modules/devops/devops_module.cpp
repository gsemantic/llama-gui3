#include "devops_module.h"
#include "devops_tools.h"
#include "devops_prompts.h"

namespace coder {
namespace devops {

namespace {

void devops_init() {
    register_devops_tools();
}

std::vector<ToolInfo> devops_get_tools() { return {}; }

std::vector<Skill> devops_get_skills() {
    return get_devops_skills();
}

const char* devops_get_system_prompt() {
    return kDevopsSystemPrompt;
}

void devops_render_panel() {}
void devops_load_settings(const std::string&) {}
void devops_save_settings() {}
void devops_shutdown() {}

} // anonymous namespace

static const CoderModule s_devops_module = {
    "devops",
    "DevOps",
    "Docker, systemd, Nginx, cron, SSH",
    devops_init,
    devops_get_tools,
    devops_get_skills,
    devops_get_system_prompt,
    devops_render_panel,
    devops_load_settings,
    devops_save_settings,
    devops_shutdown
};

void register_module() {
    ModuleRegistry::instance().register_module(&s_devops_module);
}

} // namespace devops
} // namespace coder
