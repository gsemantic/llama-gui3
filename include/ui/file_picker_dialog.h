#pragma once

#include <string>
#include <functional>
#include "file_browser.h"

namespace llama_gui {
namespace ui {

/**
 * FilePickerDialog - встроенный пикер файлов/папок (ImGui-окно).
 *
 * Это ОСНОВНОЙ и гарантированный путь выбора файлов: не зависит от внешних
 * программ (zenity/kdialog/python), поэтому работает на любом десктопе, в т.ч.
 * на минимальных live-системах. Нативный диалог (если доступен) используется
 * только как ОПЦИОНАЛЬНЫЙ ускоритель через кнопку внутри пикера.
 */
class FilePickerDialog {
public:
    using PickerCallback = std::function<void(const std::string&)>;
    using NativeAccelerator = std::function<void()>;

    enum class Mode { File, Directory };

    FilePickerDialog() = default;
    ~FilePickerDialog() = default;

    /// Начать выбор. callback вызывается с выбранным путём (или "" при отмене).
    void open(Mode mode, const std::string& title, const std::string& start_dir,
              PickerCallback callback);

    /// Отменить выбор (закрывает окно и вызывает callback с пустым путём).
    void cancel();

    /// Рендеринг окна пикера (вызывается каждый кадр из основного цикла).
    void render();

    bool is_open() const { return is_open_; }

    /// Установить опциональный ускоритель - нативный диалог.
    /// Если accelerator пустой, кнопка нативного диалога не показывается.
    void set_native_accelerator(NativeAccelerator accelerator) {
        native_accelerator_ = std::move(accelerator);
    }

private:
    void finish(const std::string& path);

    Mode mode_ = Mode::File;
    bool is_open_ = false;
    bool delivered_ = false;
    bool native_launched_ = false;
    std::string title_ = "File Picker";
    PickerCallback callback_;
    NativeAccelerator native_accelerator_;
    FileBrowser file_browser_;
};

} // namespace ui
} // namespace llama_gui
