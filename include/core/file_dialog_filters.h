#pragma once

#include <string>
#include <vector>
#include <unordered_map>

namespace llama_gui {
namespace core {

/**
 * Структура фильтра файлов для диалогов
 * Поддерживает генерацию паттернов для zenity, kdialog и Python tkinter
 */
struct FileFilter {
    std::string name;                    // Название фильтра (например, "Model files")
    std::vector<std::string> extensions; // Расширения (например, {".gguf", ".bin"})

    /**
     * Генерация паттерна для zenity
     * Формат: "Name | *.ext1 *.ext2"
     */
    std::string to_zenity_pattern() const;

    /**
     * Генерация паттерна для kdialog
     * Формат: "*.ext1 *.ext2|Name"
     */
    std::string to_kdialog_pattern() const;

    /**
     * Генерация паттерна для Python tkinter
     * Формат: [('Name', '*.ext1 *.ext2')]
     */
    std::string to_python_filetypes() const;

    /**
     * Генерация строки расширений для командной строки
     * Формат: *.ext1 *.ext2
     */
    std::string to_extensions_string() const;
};

/**
 * Менеджер фильтров файлов
 * Предоставляет централизованный доступ к предустановленным фильтрам
 */
class FileDialogFilters {
public:
    /**
     * Получить фильтр по типу
     * @param filter_type Тип фильтра: "all", "text", "model", "embedding_model", "json", "document"
     * @return Ссылка на соответствующий FileFilter
     */
    static const FileFilter& get_filter(const std::string& filter_type);

    /**
     * Проверить существование фильтра
     */
    static bool has_filter(const std::string& filter_type);

    // Предопределённые типы фильтров
    static const std::string ALL_FILES;
    static const std::string TEXT_FILES;
    static const std::string MODEL_FILES;
    static const std::string EMBEDDING_MODEL_FILES;
    static const std::string JSON_FILES;
    static const std::string DOCUMENT_FILES;

private:
    static std::unordered_map<std::string, FileFilter> filters_;
    static bool initialized_;

    // Инициализация фильтров
    static void initialize();
};

} // namespace core
} // namespace llama_gui
