#include "prompt_composer.h"

#include <algorithm>

namespace coder {

const char* prompt_layer_name(PromptLayer layer) {
    switch (layer) {
        case PromptLayer::ProjectRoot:      return "project_root";
        case PromptLayer::Base:             return "base";
        case PromptLayer::AgentRole:        return "agent_role";
        case PromptLayer::UserInstructions: return "user_instructions";
        case PromptLayer::Safety:           return "safety";
        case PromptLayer::Environment:      return "environment";
        case PromptLayer::Instructions:     return "instructions";
        case PromptLayer::Module:           return "module";
        case PromptLayer::Tools:            return "tools";
        case PromptLayer::Skills:           return "skills";
        case PromptLayer::Plan:             return "plan";
        case PromptLayer::Mode:             return "mode";
        case PromptLayer::Count:            break;
    }
    return "unknown";
}

namespace {

/* Снять пустые края: блоки приходят из разных мест, и одни уже несут
 * "\n\n" в начале (env, agent prompt), а другие нет. Без этого между
 * слоями появлялись бы тройные переводы строк, а тесты, ищущие заголовок
 * сразу после перевода строки, ловили бы не тот блок. */
std::string trim_newlines(std::string text) {
    const size_t b = text.find_first_not_of("\r\n");
    if (b == std::string::npos) return "";
    const size_t e = text.find_last_not_of("\r\n");
    return text.substr(b, e - b + 1);
}

} // namespace

std::string compose_prompt(std::vector<PromptBlock> blocks) {
    std::stable_sort(blocks.begin(), blocks.end(),
                     [](const PromptBlock& a, const PromptBlock& b) {
                         return static_cast<int>(a.layer) <
                                static_cast<int>(b.layer);
                     });
    std::string out;
    for (PromptBlock& block : blocks) {
        std::string text = trim_newlines(std::move(block.text));
        if (text.empty()) continue;
        if (!out.empty()) out += "\n\n";
        out += text;
    }
    return out;
}

} // namespace coder
