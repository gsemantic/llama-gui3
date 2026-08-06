#include "ini_section_parser.h"
#include <iostream>

namespace llama_gui {
namespace core {

int IniSectionParser::get_int(const IniParser::Document& doc, const std::string& section,
                               const std::string& key, int default_value) {
    if (!has_key(doc, section, key)) {
        return default_value;
    }

    const auto& section_map = doc.find(section);
    if (section_map == doc.end()) {
        return default_value;
    }

    try {
        return std::stoi(section_map->second.at(key));
    } catch (...) {
        std::cerr << "Warning: Failed to parse integer value for [" << section << "] " << key << std::endl;
        return default_value;
    }
}

float IniSectionParser::get_float(const IniParser::Document& doc, const std::string& section,
                                   const std::string& key, float default_value) {
    if (!has_key(doc, section, key)) {
        return default_value;
    }

    const auto& section_map = doc.find(section);
    if (section_map == doc.end()) {
        return default_value;
    }

    try {
        return std::stof(section_map->second.at(key));
    } catch (...) {
        std::cerr << "Warning: Failed to parse float value for [" << section << "] " << key << std::endl;
        return default_value;
    }
}

bool IniSectionParser::get_bool(const IniParser::Document& doc, const std::string& section,
                                 const std::string& key, bool default_value) {
    if (!has_key(doc, section, key)) {
        return default_value;
    }

    const auto& section_map = doc.find(section);
    if (section_map == doc.end()) {
        return default_value;
    }

    const std::string& value = section_map->second.at(key);
    
    if (value == "true" || value == "1" || value == "yes" || value == "on") {
        return true;
    } else if (value == "false" || value == "0" || value == "no" || value == "off") {
        return false;
    } else {
        std::cerr << "Warning: Failed to parse boolean value for [" << section << "] " << key << std::endl;
        return default_value;
    }
}

std::string IniSectionParser::get_string(const IniParser::Document& doc, const std::string& section,
                                          const std::string& key, const std::string& default_value) {
    if (!has_key(doc, section, key)) {
        return default_value;
    }

    const auto& section_map = doc.find(section);
    if (section_map == doc.end()) {
        return default_value;
    }

    return section_map->second.at(key);
}

bool IniSectionParser::has_key(const IniParser::Document& doc, const std::string& section,
                                const std::string& key) {
    const auto& section_map = doc.find(section);
    if (section_map == doc.end()) {
        return false;
    }

    return section_map->second.find(key) != section_map->second.end();
}

} // namespace core
} // namespace llama_gui
