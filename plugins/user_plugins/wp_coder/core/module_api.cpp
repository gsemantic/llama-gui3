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

void ModuleRegistry::init_all() {
    ToolsRegistry& tools = ToolsRegistry::instance();
    for (auto* mod : modules_) {
        if (!mod) continue;
        /* Владелец инструментов ставится вокруг init(): модуль регистрирует
         * свои инструменты и не обязан повторять своё имя в каждом def.
         * Снимается после — инструменты ядра, зарегистрированные позже
         * (base/git/rag), получают пустой module и видны всегда. */
        tools.set_registration_module(mod->name ? mod->name : "");
        if (mod->init) mod->init();
        tools.set_registration_module("");
    }
}

void ModuleRegistry::shutdown_all() {
    for (auto* mod : modules_) {
        if (mod->shutdown) mod->shutdown();
    }
}

} // namespace coder
