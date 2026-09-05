#include "python_module.h"
#include "python_tools.h"
#include "python_prompts.h"

namespace coder {
namespace python {

namespace {

void py_init() {
    register_python_tools();
}

std::vector<ToolInfo> py_get_tools() {
    return {};
}

std::vector<Skill> py_get_skills() {
    return get_python_skills();
}

const char* py_get_system_prompt() {
    return kPythonSystemPrompt;
}

void py_render_panel() {}
void py_load_settings(const std::string&) {}
void py_save_settings() {}
void py_shutdown() {}

} // anonymous namespace

static const CoderModule s_python_module = {
    "python",
    "Python",
    "Django, Flask, FastAPI, pip, pytest, venv",
    py_init,
    py_get_tools,
    py_get_skills,
    py_get_system_prompt,
    py_render_panel,
    py_load_settings,
    py_save_settings,
    py_shutdown
};

void register_module() {
    ModuleRegistry::instance().register_module(&s_python_module);
}

} // namespace python
} // namespace coder
