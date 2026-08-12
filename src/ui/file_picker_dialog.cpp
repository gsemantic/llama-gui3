#include "../include/ui/file_picker_dialog.h"
#include "../external/imgui/imgui.h"
#include "../include/ui/localization_manager.h"
#include <filesystem>
#include <cstring>
#include "../include/ui/input_text_context_menu.h"

namespace llama_gui {
namespace ui {

void FilePickerDialog::open(Mode mode, const std::string& title,
                            const std::string& start_dir, PickerCallback callback,
                            const std::string& default_filename) {
    mode_ = mode;
    title_ = title.empty() ? "File Picker" : title;
    callback_ = std::move(callback);
    delivered_ = false;
    native_launched_ = false;
    is_open_ = true;
    save_filename_ = default_filename;

    file_browser_.set_pick_mode(mode == Mode::Directory
                                    ? FileBrowser::PickMode::Directory
                                    : FileBrowser::PickMode::File);

    // В режиме выбора двойной клик по файлу / кнопка подтверждения завершают выбор
    file_browser_.set_file_selected_callback([this](const std::string& path) {
        finish(path);
    });

    if (!start_dir.empty()) {
        file_browser_.set_directory(start_dir);
    }
}

void FilePickerDialog::cancel() {
    finish("");
}

void FilePickerDialog::finish(const std::string& path) {
    if (delivered_) {
        return;
    }
    delivered_ = true;
    is_open_ = false;
    PickerCallback cb = std::move(callback_);
    callback_ = nullptr;
    if (cb) {
        cb(path);
    }
}

void FilePickerDialog::render() {
    if (!is_open_) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(title_.c_str(), &is_open_, ImGuiWindowFlags_NoSavedSettings)) {
        // Зарезервируем место под нижние кнопки пикера, чтобы таблица файлов
        // заполняла ровно оставшуюся высоту (единый скролл)
        float reserve = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f + 4.0f;
        if (mode_ == Mode::Save) {
            reserve += ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        }
        file_browser_.set_extra_bottom_reserve(reserve);
        file_browser_.render();
        ImGui::Separator();

        // Режим сохранения: поле ввода имени файла
        if (mode_ == Mode::Save) {
            char name_buf[256];
            strncpy(name_buf, save_filename_.c_str(), sizeof(name_buf) - 1);
            name_buf[sizeof(name_buf) - 1] = '\0';

            ImGui::Text("%s", TR("file_picker.file_name"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70.0f);
            if (ImGui::InputText("##save_filename", name_buf, sizeof(name_buf))) {
                save_filename_ = name_buf;
            }
            InputTextContextMenu();
            ImGui::SameLine();
            if (ImGui::Button(TR("file_picker.save"))) {
                if (!save_filename_.empty()) {
                    std::string dir = file_browser_.get_current_directory();
                    if (dir.empty()) {
                        dir = "/";
                    }
                    finish(dir + "/" + save_filename_);
                }
            }
            ImGui::Separator();
        }

        // Опциональный ускоритель: нативный диалог (если внешняя цепочка доступна)
        if (native_accelerator_) {
            if (ImGui::Button(TR("file_picker.native_dialog"))) {
                native_launched_ = true;
                is_open_ = false;
            }
            ImGui::SameLine();
        }

        if (ImGui::Button(TR("button.cancel"))) {
            finish("");
        }
    }
    // is_open_ мог быть сброшен кнопкой закрытия окна (X)
    if (!is_open_ && !delivered_ && !native_launched_) {
        finish("");
    }
    ImGui::End();

    // Запускаем нативный диалог после закрытия окна (гарантированный путь встроенного
    // пикера при этом не участвует в доставке результата)
    if (native_launched_) {
        native_launched_ = false;
        delivered_ = true; // результат доставит нативный диалог через enqueue_result
        NativeAccelerator accelerator = std::move(native_accelerator_);
        native_accelerator_ = nullptr;
        if (accelerator) {
            accelerator();
        }
    }
}

} // namespace ui
} // namespace llama_gui
