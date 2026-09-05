#include "module_api.h"
#include "tools_registry.h"
#include "skills_manager.h"

namespace coder {

/* ======================================================================
 * ModuleRegistry
 * ====================================================================== */

ModuleRegistry& ModuleRegistry::instance() {
    static ModuleRegistry reg;
    return reg;
}

void ModuleRegistry::register_module(const CoderModule* module) {
    if (module) modules_.push_back(module);
}

const std::vector<const CoderModule*>& ModuleRegistry::modules() const {
    return modules_;
}

const CoderModule* ModuleRegistry::find(const std::string& name) const {
    for (const auto* mod : modules_) {
        if (mod->name == name) return mod;
    }
    return nullptr;
}

std::vector<ToolInfo> ModuleRegistry::all_tools() const {
    std::vector<ToolInfo> all;
    for (const auto* mod : modules_) {
        if (mod->get_tools) {
            auto tools = mod->get_tools();
            all.insert(all.end(), tools.begin(), tools.end());
        }
    }
    return all;
}

std::vector<Skill> ModuleRegistry::all_skills() const {
    std::vector<Skill> all;
    for (const auto* mod : modules_) {
        if (mod->get_skills) {
            auto skills = mod->get_skills();
            all.insert(all.end(), skills.begin(), skills.end());
        }
    }
    return all;
}

std::string ModuleRegistry::combined_system_prompt() const {
    std::string result;
    for (const auto* mod : modules_) {
        if (mod->get_system_prompt) {
            const char* p = mod->get_system_prompt();
            if (p && p[0]) {
                if (!result.empty()) result += "\n\n";
                result += p;
            }
        }
    }
    return result;
}

void ModuleRegistry::init_all() {
    for (auto* mod : modules_) {
        if (mod->init) mod->init();
    }
}

void ModuleRegistry::shutdown_all() {
    for (auto* mod : modules_) {
        if (mod->shutdown) mod->shutdown();
    }
}

} // namespace coder
