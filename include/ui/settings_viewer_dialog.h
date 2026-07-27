#pragma once

#include <string>
#include <vector>
#include <memory>
#include "core/settings.h"
#include "core/ini_parser.h"

namespace llama_gui {
namespace ui {

using Settings = llama_gui::core::Settings;
using IniDoc = llama_gui::core::IniParser::Document;
using IniParser = llama_gui::core::IniParser;

/**
 * @brief Структура для представления строки настроек INI
 */
struct IniSettingRow {
    std::string section;
    std::string key;
    std::string value;
    std::string comment;  // Комментарий (если есть)
    bool is_modified = false;
};

/**
 * @brief Диалог просмотра и редактирования файла настроек settings.ini
 * 
 * Аналогия: php.ini viewer для web-сервера
 * Позволяет:
 * - Просматривать все настройки в виде таблицы
 * - Фильтровать по секциям и ключам
 * - Редактировать значения
 * - Сохранять изменения в файл
 * - Перезагружать настройки из файла
 */
class SettingsViewerDialog {
public:
    SettingsViewerDialog(Settings& settings);
    ~SettingsViewerDialog();

    /**
     * @brief Показать диалог
     */
    void show();

    /**
     * @brief Скрыть диалог
     */
    void hide();

    /**
     * @brief Проверка видимости диалога
     * @return true если диалог открыт
     */
    bool is_visible() const { return show_dialog_; }

    /**
     * @brief Рендеринг диалога
     */
    void render();

    /**
     * @brief Загрузить настройки из INI файла
     * @param file_path Путь к файлу (по умолчанию - текущий INI файл настроек)
     * @return true если успешно загружено
     */
    bool load_from_file(const std::string& file_path = "");

    /**
     * @brief Сохранить настройки в INI файл
     * @param file_path Путь к файлу (по умолчанию - текущий INI файл настроек)
     * @return true если успешно сохранено
     */
    bool save_to_file(const std::string& file_path = "");

    /**
     * @brief Получить количество строк настроек
     * @return Количество строк
     */
    size_t get_settings_count() const { return settings_rows_.size(); }

    /**
     * @brief Получить количество изменённых строк
     * @return Количество изменённых строк
     */
    size_t get_modified_count() const;

    /**
     * @brief Отменить все изменения
     */
    void revert_all_changes();

    /**
     * @brief Применить изменения из диалога к объекту Settings
     */
    void apply_settings_to_core();

private:
    Settings& settings_;
    bool show_dialog_ = false;
    
    // Данные INI файла
    IniDoc ini_document_;
    std::vector<IniSettingRow> settings_rows_;
    
    // Фильтры
    std::string section_filter_ = "all";  // "all" или имя секции
    std::string search_filter_ = "";       // Поиск по ключу/значению
    bool show_modified_only_ = false;      // Показывать только изменённые
    
    // Состояние UI
    bool settings_changed_ = false;
    std::string status_message_ = "";
    float scroll_y_ = 0.0f;
    
    // Список секций для dropdown
    std::vector<std::string> sections_list_;

    // Буфер для редактирования значения
    char current_edit_buffer_[1024] = "";
    int current_editing_row_index_ = -1;  // Индекс строки в settings_rows_, -1 = ничего не редактируется
    bool show_edit_modal_ = false;  // Флаг показа модального окна редактирования
    int modal_focus_frame_ = 0;  // Счётчик кадров для установки фокуса (0 = не установлен, 1 = установлен)

    /**
     * @brief Построить список строк настроек из INI документа
     */
    void build_settings_rows();

    /**
     * @brief Построить список секций для dropdown
     */
    void build_sections_list();

    /**
     * @brief Отфильтровать строки по текущим фильтрам
     * @return Отфильтрованный вектор строк
     */
    std::vector<IniSettingRow> get_filtered_rows() const;

    /**
     * @brief Рендеринг панели фильтров
     */
    void render_filter_panel();

    /**
     * @brief Рендеринг таблицы настроек
     */
    void render_settings_table();

    /**
     * @brief Рендеринг строки таблицы
     * @param original_index Индекс строки в settings_rows_
     * @param display_index Индекс строки для отображения (для ID)
     */
    void render_table_row(int original_index, int display_index);

    /**
     * @brief Рендеринг панели действий
     */
    void render_action_panel();

    /**
     * @brief Рендеринг модального окна редактирования
     */
    void render_edit_modal();
};

} // namespace ui
} // namespace llama_gui
