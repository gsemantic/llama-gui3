#pragma once

#include "bench_types.h"
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <sys/stat.h>
#include <sys/types.h>
#include <fstream>
#include <dirent.h>
#include <cmath>

namespace llama_gui {
namespace bench {

/**
 * @brief Общие вспомогательные функции для модуля бенчмарка
 */
namespace BenchCommon {

/**
 * @brief Преобразовать число в строку с разделителями тысяч
 */
inline std::string formatNumberWithSeparator(int64_t num) {
    std::string s = std::to_string(num);
    std::string result;
    int count = 0;
    
    for (int i = static_cast<int>(s.length()) - 1; i >= 0; --i) {
        if (count > 0 && count % 3 == 0) {
            result = ' ' + result;
        }
        result = s[i] + result;
        ++count;
    }
    
    return result;
}

/**
 * @brief Форматировать скорость (токенов/сек)
 */
inline std::string formatSpeed(double tps, int precision = 1) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << tps;
    return oss.str();
}

/**
 * @brief Форматировать время (мс)
 */
inline std::string formatTimeMs(double ms, int precision = 0) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << ms;
    return oss.str();
}

/**
 * @brief Форматировать длительность (секунды) в человекочитаемый вид
 */
inline std::string formatDuration(double seconds) {
    if (seconds < 60.0) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << seconds << " с";
        return oss.str();
    }
    
    int mins = static_cast<int>(seconds / 60);
    int secs = static_cast<int>(seconds) % 60;
    
    std::ostringstream oss;
    oss << mins << " мин ";
    if (secs > 0) {
        oss << secs << " с";
    }
    return oss.str();
}

/**
 * @brief Форматировать размер памяти в МБ
 */
inline std::string formatMemoryMB(size_t mb) {
    if (mb >= 1024) {
        double gb = static_cast<double>(mb) / 1024.0;
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << gb << " GB";
        return oss.str();
    }
    
    std::ostringstream oss;
    oss << mb << " MB";
    return oss.str();
}

/**
 * @brief Получить иконку статуса для UI
 */
inline const char* getStatusIcon(BenchStatus status) {
    switch (status) {
        case BenchStatus::Pending:    return "⏳";
        case BenchStatus::Running:    return "🔄";
        case BenchStatus::Completed:  return "✅";
        case BenchStatus::Failed:     return "❌";
        case BenchStatus::Cancelled:  return "⏹️";
        default:                      return "❓";
    }
}

/**
 * @brief Получить цвет для статуса (в формате ImGui)
 */
inline uint32_t getStatusColor(BenchStatus status) {
    // Формат: 0xAABBGGRR (ImGui использует ABGR в памяти)
    switch (status) {
        case BenchStatus::Pending:    return 0xFF888888;  // Серый
        case BenchStatus::Running:    return 0xFF0088FF;  // Синий
        case BenchStatus::Completed:  return 0xFF00AA00;  // Зелёный
        case BenchStatus::Failed:     return 0xFF0000FF;  // Красный
        case BenchStatus::Cancelled:  return 0xFF00AAAA;  // Бирюзовый
        default:                      return 0xFFFFFFFF;  // Белый
    }
}

/**
 * @brief Санитизировать имя файла (удалить опасные символы)
 */
inline std::string sanitizeFileName(const std::string& name) {
    std::string result = name;
    
    // Заменить опасные символы на подчёркивание
    const std::string unsafeChars = "<>:\"/\\|?*";
    for (char& c : result) {
        if (unsafeChars.find(c) != std::string::npos) {
            c = '_';
        }
    }
    
    // Удалить пробелы в начале и конце
    size_t start = result.find_first_not_of(" \t");
    size_t end = result.find_last_not_of(" \t");
    
    if (start == std::string::npos) {
        return "unnamed";
    }
    
    return result.substr(start, end - start + 1);
}

/**
 * @brief Получить текущую временную метку в формате для имён файлов
 */
inline std::string getTimestampForFileName() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&time);
    
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return oss.str();
}

/**
 * @brief Получить текущую временную метку в читаемом формате
 */
inline std::string getTimestampReadable() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&time);
    
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

/**
 * @brief Получить дату в формате для имён файлов
 */
inline std::string getDateForFileName() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&time);
    
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d");
    return oss.str();
}

/**
 * @brief Извлечь имя файла из пути
 */
inline std::string extractFileName(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos != std::string::npos) {
        return path.substr(pos + 1);
    }
    return path;
}

/**
 * @brief Извлечь расширение файла
 */
inline std::string extractExtension(const std::string& filename) {
    size_t pos = filename.find_last_of('.');
    if (pos != std::string::npos && pos < filename.length() - 1) {
        return filename.substr(pos);
    }
    return "";
}

/**
 * @brief Извлечь имя файла без расширения
 */
inline std::string extractFileNameWithoutExt(const std::string& path) {
    std::string filename = extractFileName(path);
    size_t pos = filename.find_last_of('.');
    if (pos != std::string::npos) {
        return filename.substr(0, pos);
    }
    return filename;
}

/**
 * @brief Проверить существование файла
 */
inline bool fileExists(const std::string& path) {
    std::ifstream file(path);
    return file.good();
}

/**
 * @brief Создать директорию (и родительские при необходимости)
 */
inline bool createDirectory(const std::string& path) {
    // Попытаться создать директорию
    int result = mkdir(path.c_str(), 0755);
    if (result == 0) {
        return true;
    }
    
    // Если уже существует - это нормально
    if (fileExists(path)) {
        return true;
    }
    
    // Попытаться создать родительские директории
    size_t pos = path.find_last_of('/');
    if (pos != std::string::npos && pos > 0) {
        std::string parent = path.substr(0, pos);
        if (createDirectory(parent)) {
            return mkdir(path.c_str(), 0755) == 0 || fileExists(path);
        }
    }
    
    return false;
}

/**
 * @brief Прочитать содержимое файла в строку
 */
inline std::string readFileToString(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    
    std::ostringstream oss;
    oss << file.rdbuf();
    return oss.str();
}

/**
 * @brief Записать строку в файл
 */
inline bool writeStringToFile(const std::string& path, const std::string& content) {
    // Создать родительские директории при необходимости
    size_t pos = path.find_last_of('/');
    if (pos != std::string::npos && pos > 0) {
        std::string dir = path.substr(0, pos);
        if (!createDirectory(dir)) {
            return false;
        }
    }
    
    std::ofstream file(path);
    if (!file.is_open()) {
        return false;
    }
    
    file << content;
    return file.good();
}

/**
 * @brief Получить список файлов в директории с заданным расширением
 */
inline std::vector<std::string> listFilesWithExtension(
    const std::string& directory, 
    const std::string& extension) 
{
    std::vector<std::string> files;
    
    // Простая реализация через opendir/readdir
    DIR* dir = opendir(directory.c_str());
    if (dir == nullptr) {
        return files;
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.length() > extension.length() &&
            name.compare(name.length() - extension.length(), extension.length(), extension) == 0) {
            files.push_back(directory + "/" + name);
        }
    }
    
    closedir(dir);
    
    // Сортировать по имени
    std::sort(files.begin(), files.end());
    
    return files;
}

/**
 * @brief Вычислить среднее значение вектора
 */
inline double calculateMean(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    
    double sum = 0.0;
    for (double v : values) {
        sum += v;
    }
    
    return sum / static_cast<double>(values.size());
}

/**
 * @brief Вычислить стандартное отклонение
 */
inline double calculateStdDev(
    const std::vector<double>& values, 
    double mean) 
{
    if (values.size() < 2) {
        return 0.0;
    }
    
    double sum_sq_diff = 0.0;
    for (double v : values) {
        double diff = v - mean;
        sum_sq_diff += diff * diff;
    }
    
    return std::sqrt(sum_sq_diff / static_cast<double>(values.size() - 1));
}

/**
 * @brief Найти максимальное значение
 */
inline double findMax(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    
    double max_val = values[0];
    for (double v : values) {
        if (v > max_val) {
            max_val = v;
        }
    }
    
    return max_val;
}

/**
 * @brief Найти минимальное значение
 */
inline double findMin(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    
    double min_val = values[0];
    for (double v : values) {
        if (v < min_val) {
            min_val = v;
        }
    }
    
    return min_val;
}

} // namespace BenchCommon

} // namespace bench
} // namespace llama_gui
