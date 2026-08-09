#include "../include/ui/rag_interface.h"
#include "../include/ui/rag_settings_dialog.h"
#include "../include/ui/file_dialog_helper.h"
#include "../include/core/rag_manager.h"
#include "../include/core/settings.h"
#include "../include/ui/chat_interface.h"
#include "../include/ui/localization_manager.h"
#include <imgui.h>
#include <thread>
#include <algorithm>
#include <iostream>
#include <vector>
#include <string>
#include <iomanip>
#include <sstream>

namespace {
    // Directory dialog result storage
    std::string s_pending_dir_path;
}

namespace llama_gui {
namespace ui {

RagInterface::RagInterface() = default;

RagInterface::~RagInterface() = default;

void RagInterface::show_profile_selector() {
    if (!rag_manager_) {
        ImGui::Text("RAG manager not initialized");
        return;
    }

    // Получаем список профилей
    std::vector<std::string> profiles = rag_manager_->get_index_profile_names();
    
    // ComboBox для выбора профиля
    ImGui::Text("Index Profile:");
    ImGui::SameLine();

    // Создаём массив C-string для ComboBox
    std::vector<const char*> items;
    items.reserve(profiles.size());
    for (const auto& profile : profiles) {
        items.push_back(profile.c_str());
    }

    // Текущий выбранный элемент
    static int current_item = -1;
    // Инициализируем current_item на основе текущего профиля
    if (current_item == -1 && !profiles.empty()) {
        std::string current_profile = rag_manager_->get_current_index_profile();
        if (!current_profile.empty()) {
            for (size_t i = 0; i < profiles.size(); ++i) {
                if (profiles[i] == current_profile) {
                    current_item = static_cast<int>(i);
                    break;
                }
            }
        }
    }

    ImGui::SetNextItemWidth(200);
    if (ImGui::Combo("##profile_selector", &current_item, items.data(), static_cast<int>(items.size()))) {
        // Переключаем профиль при выборе
        if (current_item >= 0 && current_item < static_cast<int>(profiles.size())) {
            rag_manager_->switch_index_profile(profiles[current_item]);
            status_message_ = "Switched to profile: " + profiles[current_item];
        }
    }

    // Определяем выбранный профиль для проверки can_delete
    bool can_delete = current_item >= 0 && current_item < static_cast<int>(profiles.size());
    std::string selected_profile = can_delete ? profiles[current_item] : "";

    ImGui::SameLine();
    if (ImGui::Button("Create")) {
        show_create_profile_dialog_ = true;
    }

    ImGui::SameLine();
    // Кнопка Delete - удаляет выбранный профиль (включая текущий, с авто-переключением)
    if (ImGui::Button("Delete", ImVec2(0, 0)) && can_delete) {
        if (rag_manager_->delete_index_profile(selected_profile, true)) {
            status_message_ = "Deleted profile: " + selected_profile;
            current_item = -1;
        } else {
            status_message_ = "Error: Cannot delete profile";
        }
    }

    ImGui::SameLine();
    // Кнопка Reindex - переиндексирует исходную директорию текущего профиля
    if (ImGui::Button("Reindex", ImVec2(0, 0)) && can_delete) {
        std::string src_dir = rag_manager_->get_current_profile_source_directory();
        if (src_dir.empty()) {
            status_message_ = "Error: Profile has no source directory";
        } else {
            processing_ = true;
            indexing_active_.store(true);
            status_message_ = "Reindexing: " + selected_profile + "...";
            std::thread([this, selected_profile]() {
                rag_manager_->reindex_current_profile();
                processing_ = false;
                indexing_active_.store(false);
                status_message_ = "Reindexed: " + selected_profile;
            }).detach();
        }
    }

    // Диалог создания профиля
    if (show_create_profile_dialog_) {
        ImGui::OpenPopup("Create Index Profile");
    }

    if (ImGui::BeginPopupModal("Create Index Profile", &show_create_profile_dialog_, ImGuiWindowFlags_AlwaysAutoResize)) {
        static char profile_name_buf[256] = "";
        static char source_dir_buf[512] = "";

        // Check for async directory dialog result every frame
        if (!s_pending_dir_path.empty()) {
            strncpy(source_dir_buf, s_pending_dir_path.c_str(), sizeof(source_dir_buf) - 1);
            source_dir_buf[sizeof(source_dir_buf) - 1] = '\0';
            s_pending_dir_path.clear();
        }

        ImGui::Text("Profile Name:");
        ImGui::InputText("##profile_name", profile_name_buf, sizeof(profile_name_buf));

        ImGui::Text("Source Directory (optional):");
        ImGui::InputText("##source_dir", source_dir_buf, sizeof(source_dir_buf));
        ImGui::SameLine();
        if (ImGui::Button("Browse...##dir", ImVec2(70, 0))) {
            FileDialogHelper helper;
            helper.open_directory_dialog("Select documents folder", [](const std::string& path) {
                s_pending_dir_path = path;
            });
        }

        ImGui::Separator();

        if (ImGui::Button("Create", ImVec2(120, 0))) {
            if (strlen(profile_name_buf) > 0) {
                std::string name = profile_name_buf;
                std::string dir = source_dir_buf;
                profile_name_buf[0] = '\0';
                source_dir_buf[0] = '\0';
                show_create_profile_dialog_ = false;
                ImGui::CloseCurrentPopup();

                // Run indexing in background thread to avoid blocking UI
                processing_ = true;
                indexing_active_.store(true);
                status_message_ = "Creating profile: " + name + "...";
                std::thread([this, name, dir]() {
                    rag_manager_->create_index_profile(name, dir);
                    processing_ = false;
                    indexing_active_.store(false);
                    status_message_ = "Created profile: " + name;
                }).detach();
                return;
            }
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            profile_name_buf[0] = '\0';
            source_dir_buf[0] = '\0';
            show_create_profile_dialog_ = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void RagInterface::reindex_with_notice() {
    if (!rag_manager_) {
        status_message_ = "RAG manager not initialized";
        return;
    }

    std::string profile = rag_manager_->get_current_index_profile();
    if (profile.empty()) {
        status_message_ = "Error: No current profile selected";
        return;
    }

    std::string src_dir = rag_manager_->get_current_profile_source_directory();
    std::vector<std::string> docs = rag_manager_->get_current_profile_documents();
    if (src_dir.empty() && docs.empty()) {
        status_message_ = "Error: Profile has no source directory or documents";
        return;
    }

    processing_ = true;
    indexing_active_.store(true);
    status_message_ = "Reindexing: " + profile + "...";
    std::thread([this, profile]() {
        rag_manager_->reindex_current_profile();
        processing_ = false;
        indexing_active_.store(false);
        status_message_ = "Reindexed: " + profile;
    }).detach();
}

void RagInterface::sync_rag_state_with_chat() {
    // Синхронизируем состояние RAG между RagInterface и ChatInterface
    if (chat_interface_) {
        // Проверяем, изменилось ли состояние
        bool chat_rag_enabled = chat_interface_->is_rag_enabled();
        if (rag_enabled_ != chat_rag_enabled) {
            // Обновляем локальное состояние
            rag_enabled_ = chat_rag_enabled;
            std::cout << "RagInterface: Synced RAG state from ChatInterface (enabled=" << rag_enabled_ << ")" << std::endl;
        }
    }
}

void RagInterface::render_ui(bool* visible) {
    // Стандартное окно ImGui с заголовком и кнопками управления
    // Кнопка закрытия (×) и сворачивания (─) рисуются автоматически ImGui
    if (!ImGui::Begin("RAG", visible, ImGuiWindowFlags_None)) {
        ImGui::End();
        return;
    }

    // Синхронизируем состояние при каждом кадре
    sync_rag_state_with_chat();

    // === Селектор профилей индексов ===
    ImGui::Separator();
    show_profile_selector();
    ImGui::Separator();

    // === Уведомление о необходимости переиндексации ===
    // Индекс построен другой моделью эмбеддингов или размерностью — векторы
    // несопоставимы, поиск будет неверным. Информируем пользователя.
    if (rag_manager_ && rag_manager_->needs_reindex()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
        ImGui::TextWrapped("⚠ %s", rag_manager_->get_reindex_reason().c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Reindex now")) {
            reindex_with_notice();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Переиндексирует текущий профиль активной моделью эмбеддингов.\n"
                              "Профиль должен иметь source directory или документы.");
        }
        ImGui::Separator();
    }

    bool prev_rag_enabled = rag_enabled_;
    ImGui::Checkbox("Enable RAG", &rag_enabled_);

    // Если состояние изменилось, синхронизируем с ChatInterface и сохраняем настройки
    if (prev_rag_enabled != rag_enabled_ && chat_interface_) {
        chat_interface_->enable_rag(rag_enabled_);
        std::cout << "RagInterface: RAG " << (rag_enabled_ ? "включен" : "выключен") << " (синхронизация с ChatInterface)" << std::endl;
        
        // Сохраняем настройки в профиль
        if (settings_) {
            std::string profile_name = settings_->get_current_profile_name();
            if (settings_->save_profile(profile_name)) {
                std::cout << "✓ RAG settings saved to profile: " << profile_name << std::endl;
            } else {
                std::cerr << "✗ Failed to save RAG settings to profile: " << profile_name << std::endl;
            }
        }
    }

    if (ImGui::Button("Load Documents")) {
        handle_document_upload();
    }

    ImGui::SameLine();
    if (ImGui::Button("Clear Documents")) {
        clear_documents();
    }

    ImGui::SameLine();
    if (ImGui::Button("RAG Settings")) {
        // Открываем диалог настроек RAG
        if (rag_settings_dialog_) {
            rag_settings_dialog_->set_visible(true);
        }
    }

    // === ПЕРСИСТЕНТНОСТЬ: Кнопки управления индексом ===
    ImGui::Separator();
    ImGui::Text(TR("rag.persistence"));
    
    if (ImGui::Button(TR("rag.save_index"))) {
        save_index();
    }
    ImGui::SameLine();
    
    if (ImGui::Button(TR("rag.load_index"))) {
        load_index();
    }
    ImGui::SameLine();
    
    if (ImGui::Button(TR("rag.clear_index"))) {
        clear_index();
    }

    // Показываем прогресс обработки
    if (processing_ || indexing_active_.load()) {
        render_indexing_progress();
    }

    // Show indexing result (after completion)
    render_indexing_result();

    // Показываем статус (only when not showing progress/result)
    if (!status_message_.empty() && !processing_ && !indexing_active_.load() && !show_indexing_result_) {
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", status_message_.c_str());
    }

    // === Отображение статуса персистентности ===
    ImGui::Separator();
    ImGui::Text(TR("rag.index_status"));
    
    // Показываем информацию о загруженных чанках
    ImGui::Text(TR("rag.loaded_chunks"), get_loaded_chunks_count());
    
    // Показываем путь к персистентному индексу
    std::string index_path = get_persistent_index_path();
    bool index_exists = persistent_index_exists();

    char index_info[512];
    snprintf(index_info, sizeof(index_info), "%s %s", TR("rag.index_file"), index_path.c_str());
    ImGui::Text("%s", index_info);
    ImGui::SameLine();
    if (index_exists) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", TR("rag.index_exists"));
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "%s", TR("rag.index_not_found"));
    }

    // Показываем загруженные документы
    if (!loaded_documents_.empty()) {
        ImGui::Separator();
        ImGui::Text("Loaded Documents (%zu):", loaded_documents_.size());

        for (size_t i = 0; i < loaded_documents_.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));

            std::string filename = get_filename_from_path(loaded_documents_[i]);
            ImGui::Text("%s", filename.c_str());

            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                handle_document_remove(static_cast<int>(i));
            }

            ImGui::PopID();
        }
    }

    ImGui::End();
}

void RagInterface::handle_document_upload() {
    // Для простоты будем использовать один файл за раз
    // В реальной реализации можно добавить поддержку выбора нескольких файлов
    std::string file_path;
    
    // Используем существующий метод для выбора одного файла
    // Для этого нужно создать временную переменную и использовать callback
    auto callback = [&file_path](const std::string& path) {
        file_path = path;
    };
    
    // Вызываем метод FileDialogHelper для выбора файла
    // Используем существующую реализацию
    FileDialogHelper helper;
    helper.open_file_dialog("Select document", [this, callback](const std::string& path) {
        if (!path.empty()) {
            loaded_documents_.push_back(path);
            
            // Асинхронно обрабатываем документы
            std::thread processing_thread(&RagInterface::process_uploaded_documents, this);
            processing_thread.detach();
        }
    });
}

void RagInterface::process_uploaded_documents() {
    processing_ = true;
    progress_ = 0.0f;
    current_operation_ = "Processing";
    indexing_active_.store(true);
    indexing_progress_value_.store(0.0f);
    show_indexing_result_ = false;

    // Обновляем статус
    status_message_ = "Processing documents...";

    // Получаем RagManager через ссылку
    if (rag_manager_) {
        rag_manager_->process_documents_batch(loaded_documents_);
    }

    processing_ = false;
    current_operation_ = "";
    indexing_active_.store(false);
}

void RagInterface::on_indexing_progress(const llama_gui::core::IndexingProgress& progress) {
    // Update atomic values for thread-safe access from ChatInterface
    indexing_phase_ = progress.phase;
    last_indexing_progress_ = progress;
    indexing_status_ = progress.status_message;

    switch (progress.phase) {
        case llama_gui::core::IndexingPhase::Complete:
            indexing_active_.store(false);
            indexing_progress_value_.store(1.0f);
            status_message_ = progress.status_message;
            // Store final stats for result display
            show_indexing_result_ = true;
            result_file_count_ = progress.final_file_count;
            result_chunk_count_ = progress.final_chunk_count;
            result_elapsed_seconds_ = progress.elapsed_seconds;
            break;
        case llama_gui::core::IndexingPhase::Error:
            indexing_active_.store(false);
            status_message_ = "Error: " + progress.error_message;
            break;
        default:
            indexing_active_.store(true);
            indexing_progress_value_.store(progress.overall_progress);
            status_message_ = progress.status_message;
            break;
    }
}

void RagInterface::render_indexing_progress() {
    if (!processing_ && !indexing_active_.load()) return;

    llama_gui::core::IndexingProgress p = last_indexing_progress_;

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f), "Indexing in progress...");
    ImGui::Spacing();

    // Phase indicators
    auto phase_icon = [](llama_gui::core::IndexingPhase phase, llama_gui::core::IndexingPhase target) -> const char* {
        if (phase == target) return "[*]";  // Currently active
        if (static_cast<int>(phase) > static_cast<int>(target)) return "[v]";  // Completed
        return "[ ]";  // Not yet started
    };

    auto phase_color = [](llama_gui::core::IndexingPhase phase, llama_gui::core::IndexingPhase target) -> ImVec4 {
        if (phase == target) return ImVec4(0.2f, 0.8f, 1.0f, 1.0f);  // Active - blue
        if (static_cast<int>(phase) > static_cast<int>(target)) return ImVec4(0.3f, 1.0f, 0.3f, 1.0f);  // Done - green
        return ImVec4(0.5f, 0.5f, 0.5f, 1.0f);  // Pending - gray
    };

    ImGui::Indent(10.0f);

    // 1. Parsing
    ImGui::TextColored(phase_color(p.phase, llama_gui::core::IndexingPhase::Parsing),
                       "%s Parsing", phase_icon(p.phase, llama_gui::core::IndexingPhase::Parsing));
    if (p.phase == llama_gui::core::IndexingPhase::Parsing && !p.current_file.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", get_filename_from_path(p.current_file).c_str());
    }

    // 2. Chunking
    ImGui::TextColored(phase_color(p.phase, llama_gui::core::IndexingPhase::Chunking),
                       "%s Chunking", phase_icon(p.phase, llama_gui::core::IndexingPhase::Chunking));

    // 3. Embedding
    ImGui::TextColored(phase_color(p.phase, llama_gui::core::IndexingPhase::Embedding),
                       "%s Embedding", phase_icon(p.phase, llama_gui::core::IndexingPhase::Embedding));
    if (p.phase == llama_gui::core::IndexingPhase::Embedding && p.total_chunks > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%d/%d chunks)", p.current_chunk, p.total_chunks);
    }

    // 4. Saving
    ImGui::TextColored(phase_color(p.phase, llama_gui::core::IndexingPhase::Saving),
                       "%s Saving", phase_icon(p.phase, llama_gui::core::IndexingPhase::Saving));

    ImGui::Unindent(10.0f);

    ImGui::Spacing();

    // Overall progress bar
    float progress_val = indexing_progress_value_.load();
    ImGui::ProgressBar(progress_val, ImVec2(-1, 0));

    // File counter
    if (p.total_files > 0) {
        ImGui::Text("File %d/%d", p.current_file_index + 1, p.total_files);
    }

    // Current file name
    if (!p.current_file.empty()) {
        ImGui::TextDisabled("%s", get_filename_from_path(p.current_file).c_str());
    }

    // Elapsed time
    if (p.elapsed_seconds > 0) {
        int mins = static_cast<int>(p.elapsed_seconds) / 60;
        int secs = static_cast<int>(p.elapsed_seconds) % 60;
        ImGui::SameLine(ImGui::GetWindowWidth() - 80);
        ImGui::TextDisabled("%dm %ds", mins, secs);
    }
}

void RagInterface::render_indexing_result() {
    if (!show_indexing_result_) return;

    ImGui::Separator();

    // Green success indicator
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.3f, 1.0f), "Indexing Complete");
    ImGui::Spacing();

    ImGui::Indent(10.0f);

    // Stats
    ImGui::Text("Files:   %d", result_file_count_);
    ImGui::Text("Chunks:  %d", result_chunk_count_);

    int mins = static_cast<int>(result_elapsed_seconds_) / 60;
    int secs = static_cast<int>(result_elapsed_seconds_) % 60;
    ImGui::Text("Time:    %dm %02ds", mins, secs);

    // Index size: use actual chunk count and embedding dimension (384d float = 1536 bytes per vector)
    // Plus metadata overhead per chunk (~200 bytes for content + metadata)
    float estimated_size_mb = (result_chunk_count_ * (1536.0f + 200.0f)) / (1024.0f * 1024.0f);
    ImGui::Text("Size:    ~%.2f MB", estimated_size_mb);

    ImGui::Unindent(10.0f);

    ImGui::Spacing();

    // Dismiss button
    if (ImGui::Button("OK")) {
        show_indexing_result_ = false;
    }
}

void RagInterface::handle_document_remove(int index) {
    if (index >= 0 && index < static_cast<int>(loaded_documents_.size())) {
        loaded_documents_.erase(loaded_documents_.begin() + index);
        status_message_ = "Document removed";
    }
}

void RagInterface::clear_documents() {
    loaded_documents_.clear();
    status_message_ = "Documents cleared";
}

// === Персистентность: реализация методов управления индексом ===
void RagInterface::save_index() {
    if (rag_manager_) {
        if (rag_manager_->save_index()) {
            status_message_ = "Index saved successfully!";
            std::cout << "[RAG UI] Index saved successfully" << std::endl;
        } else {
            status_message_ = "Failed to save index";
            std::cerr << "[RAG UI] Failed to save index" << std::endl;
        }
    } else {
        status_message_ = "RAG manager not initialized";
        std::cerr << "[RAG UI] Cannot save index: RAG manager not initialized" << std::endl;
    }
}

void RagInterface::load_index() {
    if (rag_manager_) {
        if (rag_manager_->load_index()) {
            status_message_ = "Index loaded successfully!";
            std::cout << "[RAG UI] Index loaded successfully" << std::endl;
        } else {
            status_message_ = "Failed to load index (file may not exist)";
            std::cerr << "[RAG UI] Failed to load index" << std::endl;
        }
    } else {
        status_message_ = "RAG manager not initialized";
        std::cerr << "[RAG UI] Cannot load index: RAG manager not initialized" << std::endl;
    }
}

void RagInterface::clear_index() {
    if (rag_manager_) {
        rag_manager_->clear_all_indexes();
        status_message_ = "Index cleared";
        std::cout << "[RAG UI] Index cleared" << std::endl;
    } else {
        status_message_ = "RAG manager not initialized";
        std::cerr << "[RAG UI] Cannot clear index: RAG manager not initialized" << std::endl;
    }
}

size_t RagInterface::get_loaded_chunks_count() {
    if (rag_manager_) {
        return rag_manager_->get_external_chunks_count();
    }
    return 0;
}

std::string RagInterface::get_persistent_index_path() {
    if (rag_manager_) {
        // Получаем путь из текущего профиля
        std::string index_path = rag_manager_->get_current_index_path();
        
        // Если профиль не установлен или путь пустой, используем путь по умолчанию
        if (index_path.empty()) {
            const char* home = getenv("HOME");
            if (!home) {
                home = ".";
            }
            return std::string(home) + "/.llama-gui/rag_indexes/rag_index.faiss";
        }
        
        return index_path;
    }
    return "";
}

bool RagInterface::persistent_index_exists() {
    if (rag_manager_) {
        return rag_manager_->has_persistent_index();
    }
    return false;
}

std::string RagInterface::get_filename_from_path(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos != std::string::npos) {
        return path.substr(pos + 1);
    }
    return path;
}

std::vector<std::string> RagInterface::get_supported_extensions() {
    return {".txt"}; // Пока поддерживаем только текстовые файлы
    // В будущем можно добавить: {".txt", ".pdf", ".docx"}
}

void RagInterface::set_rag_manager(llama_gui::core::RagManager* rag_manager) {
    rag_manager_ = rag_manager;

    // Subscribe to indexing progress callbacks
    if (rag_manager_) {
        rag_manager_->set_indexing_progress_callback(
            [this](const llama_gui::core::IndexingProgress& progress) {
                on_indexing_progress(progress);
            });
    }
}

void RagInterface::update_settings_from_manager() {
    // Этот метод будет вызываться для обновления настроек из RAG-менеджера
    // В текущей реализации он может быть использован для синхронизации состояния
    if (rag_manager_) {
        // Пример: обновление состояния включения RAG
        // В реальной реализации здесь может быть больше логики
    }
}

} // namespace ui
} // namespace llama_gui