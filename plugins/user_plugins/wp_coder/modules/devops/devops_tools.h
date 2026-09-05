#pragma once

/*
 * devops_tools.h — DevOps-специфичные инструменты.
 */

#include "../../core/module_api.h"
#include <vector>

namespace coder {
namespace devops {

void register_devops_tools();
std::vector<Skill> get_devops_skills();

} // namespace devops
} // namespace coder
