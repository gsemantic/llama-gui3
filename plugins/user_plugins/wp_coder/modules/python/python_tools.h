#pragma once

/*
 * python_tools.h — Python-специфичные инструменты.
 */

#include "../../core/module_api.h"
#include <vector>

namespace coder {
namespace python {

void register_python_tools();
std::vector<Skill> get_python_skills();

} // namespace python
} // namespace coder
