#pragma once

#include "ini_parser.h"
#include <string>

namespace llama_gui {
namespace core {

/**
 * @brief Parser для парсинга секций INI файла
 *
 * Предоставляет методы для извлечения значений из секций
 * с автоматической конвертацией типов
 */
class IniSectionParser {
public:
    /**
     * @brief Извлечь целочисленное значение из секции
     */
    static int get_int(const IniParser::Document& doc, const std::string& section,
                       const std::string& key, int default_value = 0);

    /**
     * @brief Извлечь значение с плавающей точкой из секции
     */
    static float get_float(const IniParser::Document& doc, const std::string& section,
                           const std::string& key, float default_value = 0.0f);

    /**
     * @brief Извлечь булево значение из секции
     */
    static bool get_bool(const IniParser::Document& doc, const std::string& section,
                         const std::string& key, bool default_value = false);

    /**
     * @brief Извлечь строковое значение из секции
     */
    static std::string get_string(const IniParser::Document& doc, const std::string& section,
                                  const std::string& key, const std::string& default_value = "");

    /**
     * @brief Проверить наличие ключа в секции
     */
    static bool has_key(const IniParser::Document& doc, const std::string& section,
                        const std::string& key);
};

} // namespace core
} // namespace llama_gui
