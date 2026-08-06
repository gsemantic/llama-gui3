#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace llama_gui {
namespace core {

/**
 * @brief Простой INI парсер для файла настроек settings.ini
 * 
 * Поддерживает:
 * - Секции [section]
 * - Ключи key=value
 * - Комментарии (; и #)
 * - Пустые строки
 */
class IniParser {
public:
    using Section = std::unordered_map<std::string, std::string>;
    using Document = std::unordered_map<std::string, Section>;

    /**
     * @brief Загрузить INI файл
     * @param file_path Путь к файлу
     * @return true если успешно загружен
     */
    static bool load(const std::string& file_path, Document& doc) {
        std::ifstream file(file_path);
        if (!file.is_open()) {
            return false;
        }

        std::string line;
        std::string current_section = "default";
        doc[current_section] = Section();

        while (std::getline(file, line)) {
            // Trim whitespace
            line = trim(line);

            // Skip empty lines and comments
            if (line.empty() || line[0] == ';' || line[0] == '#') {
                continue;
            }

            // Check for section header
            if (line[0] == '[' && line.back() == ']') {
                current_section = line.substr(1, line.size() - 2);
                current_section = trim(current_section);
                if (doc.find(current_section) == doc.end()) {
                    doc[current_section] = Section();
                }
                continue;
            }

            // Parse key=value
            auto pos = line.find('=');
            if (pos != std::string::npos) {
                std::string key = trim(line.substr(0, pos));
                std::string value = trim(line.substr(pos + 1));
                doc[current_section][key] = value;
            }
        }

        file.close();
        return true;
    }

    /**
     * @brief Сохранить INI файл
     * @param file_path Путь к файлу
     * @param doc Документ для сохранения
     * @param header Заголовок файла (комментарии в начале)
     * @return true если успешно сохранен
     */
    static bool save(const std::string& file_path, const Document& doc, const std::string& header = "") {
        std::ofstream file(file_path);
        if (!file.is_open()) {
            return false;
        }

        // Write header
        if (!header.empty()) {
            file << header;
        }

        // Write sections
        for (const auto& [section, values] : doc) {
            if (section == "default") {
                // Default section without name
                for (const auto& [key, value] : values) {
                    file << key << " = " << value << "\n";
                }
            } else {
                // Named section
                file << "\n[" << section << "]\n";
                for (const auto& [key, value] : values) {
                    file << key << " = " << value << "\n";
                }
            }
        }

        file.close();
        return true;
    }

    /**
     * @brief Получить значение из секции
     * @param doc Документ
     * @param section Секция
     * @param key Ключ
     * @param default_value Значение по умолчанию
     * @return Значение или default_value если не найдено
     */
    static std::string get(const Document& doc, const std::string& section, 
                          const std::string& key, const std::string& default_value = "") {
        auto sec_it = doc.find(section);
        if (sec_it == doc.end()) return default_value;
        
        auto key_it = sec_it->second.find(key);
        if (key_it == sec_it->second.end()) return default_value;
        
        return key_it->second;
    }

    /**
     * @brief Установить значение в секции
     * @param doc Документ
     * @param section Секция
     * @param key Ключ
     * @param value Значение
     */
    static void set(Document& doc, const std::string& section, 
                   const std::string& key, const std::string& value) {
        doc[section][key] = value;
    }

    /**
     * @brief Получить значение как integer
     */
    static int get_int(const Document& doc, const std::string& section,
                      const std::string& key, int default_value = 0) {
        std::string val = get(doc, section, key, "");
        if (val.empty()) return default_value;
        try {
            return std::stoi(val);
        } catch (...) {
            return default_value;
        }
    }

    /**
     * @brief Получить значение как float
     */
    static float get_float(const Document& doc, const std::string& section,
                          const std::string& key, float default_value = 0.0f) {
        std::string val = get(doc, section, key, "");
        if (val.empty()) return default_value;
        try {
            return std::stof(val);
        } catch (...) {
            return default_value;
        }
    }

    /**
     * @brief Получить значение как bool
     */
    static bool get_bool(const Document& doc, const std::string& section,
                        const std::string& key, bool default_value = false) {
        std::string val = get(doc, section, key, "");
        if (val.empty()) return default_value;
        
        // Convert to lowercase for comparison
        std::string lower_val = val;
        std::transform(lower_val.begin(), lower_val.end(), lower_val.begin(), 
                      [](unsigned char c) { return std::tolower(c); });
        
        return (lower_val == "true" || lower_val == "1" || lower_val == "yes" || lower_val == "on");
    }

    /**
     * @brief Получить значение как vector строк (разделитель - запятая)
     */
    static std::vector<std::string> get_string_list(const Document& doc, const std::string& section,
                                                    const std::string& key, 
                                                    const std::vector<std::string>& default_value = {}) {
        std::string val = get(doc, section, key, "");
        if (val.empty()) return default_value;
        
        std::vector<std::string> result;
        std::stringstream ss(val);
        std::string item;
        
        while (std::getline(ss, item, ',')) {
            item = trim(item);
            if (!item.empty()) {
                result.push_back(item);
            }
        }
        
        return result;
    }

private:
    static std::string trim(const std::string& str) {
        size_t start = str.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        size_t end = str.find_last_not_of(" \t\r\n");
        return str.substr(start, end - start + 1);
    }
};

} // namespace core
} // namespace llama_gui
