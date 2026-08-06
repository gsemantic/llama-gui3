#include "../include/ui/grid_snapping_system.h"
#include <cmath>
#include <iostream>
#include <fstream>

#ifdef USE_IMGUI
#include "../external/imgui/imgui.h"
#endif

namespace llama_gui {
namespace ui {

// =========================================================================
// Конструктор/деструктор
// =========================================================================

GridSnappingSystem::GridSnappingSystem() = default;
GridSnappingSystem::~GridSnappingSystem() = default;

// =========================================================================
// Основные методы
// =========================================================================

#ifdef USE_IMGUI
ImVec2 GridSnappingSystem::snapPosition(const ImVec2& position) const {
    if (!settings_.enabled || !settings_.snap_position) {
        return position;
    }

    // Проверка кэша для оптимизации
    if (position_cache_.valid && 
        position_cache_.last_grid_size == settings_.grid_size &&
        position_cache_.last_input.x == position.x && 
        position_cache_.last_input.y == position.y) {
        return position_cache_.last_result;
    }

    float grid_size = static_cast<float>(settings_.grid_size);
    ImVec2 result;
    
    if (settings_.enable_fine_tuning) {
        // Режим точной подстройки: используем мелкую сетку
        float fine_grid = grid_size / static_cast<float>(settings_.fine_grid_divisor);
        result.x = std::round(position.x / fine_grid) * fine_grid;
        result.y = std::round(position.y / fine_grid) * fine_grid;
    } else {
        // Обычный режим: основная сетка
        result.x = std::round(position.x / grid_size) * grid_size;
        result.y = std::round(position.y / grid_size) * grid_size;
    }

    // Обновление кэша
    position_cache_.valid = true;
    position_cache_.last_grid_size = settings_.grid_size;
    position_cache_.last_input = position;
    position_cache_.last_result = result;

    return result;
}

ImVec2 GridSnappingSystem::snapSize(const ImVec2& size) const {
    if (!settings_.enabled || !settings_.snap_size) {
        return size;
    }

    // Проверка кэша для оптимизации
    if (size_cache_.valid && 
        size_cache_.last_grid_size == settings_.grid_size &&
        size_cache_.last_input.x == size.x && 
        size_cache_.last_input.y == size.y) {
        return size_cache_.last_result;
    }

    float grid_size = static_cast<float>(settings_.grid_size);
    ImVec2 result;
    
    if (settings_.enable_fine_tuning) {
        // Режим точной подстройки: используем мелкую сетку
        float fine_grid = grid_size / static_cast<float>(settings_.fine_grid_divisor);
        result.x = std::max(fine_grid * 2, std::round(size.x / fine_grid) * fine_grid);
        result.y = std::max(fine_grid * 2, std::round(size.y / fine_grid) * fine_grid);
    } else {
        // Обычный режим: основная сетка
        result.x = std::max(grid_size * 2, std::round(size.x / grid_size) * grid_size);
        result.y = std::max(grid_size * 2, std::round(size.y / grid_size) * grid_size);
    }

    // Обновление кэша
    size_cache_.valid = true;
    size_cache_.last_grid_size = settings_.grid_size;
    size_cache_.last_input = size;
    size_cache_.last_result = result;

    return result;
}

ImVec2 GridSnappingSystem::snapPositionWithThreshold(const ImVec2& position, float threshold) const {
    if (!settings_.enabled || !settings_.snap_position) {
        return position;
    }

    if (threshold <= 0.0f) {
        threshold = settings_.snap_threshold;
    }

    float grid_size = static_cast<float>(settings_.grid_size);
    float fine_grid = settings_.enable_fine_tuning ? 
                      grid_size / static_cast<float>(settings_.fine_grid_divisor) : grid_size;
    
    ImVec2 result;
    
    // Вычисляем ближайшую позицию сетки
    float snapped_x = std::round(position.x / fine_grid) * fine_grid;
    float snapped_y = std::round(position.y / fine_grid) * fine_grid;
    
    // Применяем порог: если разница меньше порога, примагничиваем
    float diff_x = std::abs(snapped_x - position.x);
    float diff_y = std::abs(snapped_y - position.y);
    
    result.x = (diff_x <= threshold) ? snapped_x : position.x;
    result.y = (diff_y <= threshold) ? snapped_y : position.y;

    return result;
}
#endif

float GridSnappingSystem::snapToGrid(float value) const {
    if (!settings_.enabled) {
        return value;
    }

    float grid_size = static_cast<float>(settings_.grid_size);
    
    if (settings_.enable_fine_tuning) {
        float fine_grid = grid_size / static_cast<float>(settings_.fine_grid_divisor);
        return std::round(value / fine_grid) * fine_grid;
    }
    
    return std::round(value / grid_size) * grid_size;
}

float GridSnappingSystem::snapSizeToGrid(float value) const {
    if (!settings_.enabled) {
        return value;
    }

    float grid_size = static_cast<float>(settings_.grid_size);
    float min_size = settings_.enable_fine_tuning ? 
                     (grid_size / static_cast<float>(settings_.fine_grid_divisor)) * 2 : 
                     grid_size * 2;
    
    if (settings_.enable_fine_tuning) {
        float fine_grid = grid_size / static_cast<float>(settings_.fine_grid_divisor);
        return std::max(min_size, std::round(value / fine_grid) * fine_grid);
    }
    
    return std::max(min_size, std::round(value / grid_size) * grid_size);
}

// =========================================================================
// Настройки
// =========================================================================

void GridSnappingSystem::setSettings(const GridSnappingSettings& settings) {
    settings_ = settings;
    
    // Сброс кэша при изменении настроек
    position_cache_.valid = false;
    size_cache_.valid = false;
    
    std::cout << "GridSnappingSystem: Settings updated (enabled: " 
              << (settings_.enabled ? "yes" : "no") 
              << ", grid_size: " << settings_.grid_size 
              << ", fine_tuning: " << (settings_.enable_fine_tuning ? "yes" : "no")
              << ")" << std::endl;
}

void GridSnappingSystem::setEnabled(bool enabled) {
    settings_.enabled = enabled;
    position_cache_.valid = false;
    size_cache_.valid = false;
    std::cout << "GridSnappingSystem: " << (enabled ? "Enabled" : "Disabled") << std::endl;
}

void GridSnappingSystem::setGridSize(int grid_size) {
    settings_.grid_size = std::max(4, grid_size); // Минимум 4 пикселя
    position_cache_.valid = false;
    size_cache_.valid = false;
    std::cout << "GridSnappingSystem: Grid size set to " << settings_.grid_size << "px" << std::endl;
}

void GridSnappingSystem::setFineTuningMode(bool enabled, int divisor) {
    settings_.enable_fine_tuning = enabled;
    settings_.fine_grid_divisor = std::clamp(divisor, 2, 16);
    position_cache_.valid = false;
    size_cache_.valid = false;
    std::cout << "GridSnappingSystem: Fine-tuning " << (enabled ? "enabled" : "disabled")
              << " (divisor: " << settings_.fine_grid_divisor << ")" << std::endl;
}

int GridSnappingSystem::getFineGridSize() const {
    if (!settings_.enable_fine_tuning) {
        return settings_.grid_size;
    }
    return settings_.grid_size / settings_.fine_grid_divisor;
}

void GridSnappingSystem::setShowGridOverlay(bool show) {
    settings_.show_grid_overlay = show;
    std::cout << "GridSnappingSystem: Grid overlay " << (show ? "enabled" : "disabled") << std::endl;
}

// =========================================================================
// Визуализация
// =========================================================================

void GridSnappingSystem::renderGridOverlay(int viewport_width, int viewport_height) const {
    if (!settings_.enabled || !settings_.show_grid_overlay) {
        return;
    }

#ifdef USE_IMGUI
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    if (!draw_list) return;

    float grid_size = static_cast<float>(settings_.grid_size);
    float fine_grid = settings_.enable_fine_tuning ? 
                      grid_size / static_cast<float>(settings_.fine_grid_divisor) : grid_size;
    
    // Основные линии сетки (более яркие)
    ImU32 major_color = IM_COL32(100, 100, 100, 80);  // Серый, полупрозрачный
    // Мелкие линии сетки (более тусклые)
    ImU32 minor_color = IM_COL32(80, 80, 80, 40);     // Тёмно-серый, более прозрачный
    
    // Вертикальные линии
    for (float x = 0; x <= viewport_width; x += grid_size) {
        // Основная линия
        draw_list->AddLine(ImVec2(x, 0), ImVec2(x, static_cast<float>(viewport_height)), 
                          major_color, 1.0f);
        
        // Мелкие линии (если включена точная подстройка)
        if (settings_.enable_fine_tuning) {
            for (int i = 1; i < settings_.fine_grid_divisor; ++i) {
                float fine_x = x + i * fine_grid;
                if (fine_x <= viewport_width) {
                    draw_list->AddLine(ImVec2(fine_x, 0), 
                                      ImVec2(fine_x, static_cast<float>(viewport_height)), 
                                      minor_color, 0.5f);
                }
            }
        }
    }
    
    // Горизонтальные линии
    for (float y = 0; y <= viewport_height; y += grid_size) {
        // Основная линия
        draw_list->AddLine(ImVec2(0, y), ImVec2(static_cast<float>(viewport_width), y), 
                          major_color, 1.0f);
        
        // Мелкие линии (если включена точная подстройка)
        if (settings_.enable_fine_tuning) {
            for (int i = 1; i < settings_.fine_grid_divisor; ++i) {
                float fine_y = y + i * fine_grid;
                if (fine_y <= viewport_height) {
                    draw_list->AddLine(ImVec2(0, fine_y), 
                                      ImVec2(static_cast<float>(viewport_width), fine_y), 
                                      minor_color, 0.5f);
                }
            }
        }
    }
#endif
}

#ifdef USE_IMGUI
void GridSnappingSystem::renderSnapIndicator(const ImVec2& window_pos, const ImVec2& window_size) const {
    if (!settings_.enabled || !settings_.show_grid_overlay) {
        return;
    }

#ifdef USE_IMGUI
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    if (!draw_list) return;

    // Проверяем, выровнена ли позиция
    float fine_grid = settings_.enable_fine_tuning ? 
                      static_cast<float>(getFineGridSize()) : 
                      static_cast<float>(settings_.grid_size);
    
    bool pos_snapped_x = (std::fmod(window_pos.x, fine_grid) < 1.0f);
    bool pos_snapped_y = (std::fmod(window_pos.y, fine_grid) < 1.0f);
    bool size_snapped_x = (std::fmod(window_size.x, fine_grid) < 1.0f);
    bool size_snapped_y = (std::fmod(window_size.y, fine_grid) < 1.0f);

    ImU32 snapped_color = IM_COL32(0, 255, 0, 200);    // Зелёный
    ImU32 not_snapped_color = IM_COL32(255, 100, 0, 200); // Оранжевый

    // Индикатор в левом верхнем углу окна
    float indicator_x = window_pos.x + 5;
    float indicator_y = window_pos.y + window_size.y - 25;
    
    // Позиция
    draw_list->AddText(ImVec2(indicator_x, indicator_y), 
                      pos_snapped_x && pos_snapped_y ? snapped_color : not_snapped_color,
                      pos_snapped_x && pos_snapped_y ? "✓ Pos" : "✗ Pos");
    
    // Размер
    draw_list->AddText(ImVec2(indicator_x + 50, indicator_y), 
                      size_snapped_x && size_snapped_y ? snapped_color : not_snapped_color,
                      size_snapped_x && size_snapped_y ? "✓ Size" : "✗ Size");
#endif
}
#endif

// =========================================================================
// Сервисные методы
// =========================================================================

bool GridSnappingSystem::saveSettingsToFile(const std::string& filepath) const {
    try {
        std::ofstream file(filepath);
        if (!file.is_open()) {
            std::cerr << "GridSnappingSystem: Cannot open file for writing: " << filepath << std::endl;
            return false;
        }

        file << "{\n";
        file << "  \"enabled\": " << (settings_.enabled ? "true" : "false") << ",\n";
        file << "  \"grid_size\": " << settings_.grid_size << ",\n";
        file << "  \"snap_position\": " << (settings_.snap_position ? "true" : "false") << ",\n";
        file << "  \"snap_size\": " << (settings_.snap_size ? "true" : "false") << ",\n";
        file << "  \"show_grid_overlay\": " << (settings_.show_grid_overlay ? "true" : "false") << ",\n";
        file << "  \"snap_threshold\": " << settings_.snap_threshold << ",\n";
        file << "  \"enable_fine_tuning\": " << (settings_.enable_fine_tuning ? "true" : "false") << ",\n";
        file << "  \"fine_grid_divisor\": " << settings_.fine_grid_divisor << "\n";
        file << "}\n";

        file.close();
        std::cout << "GridSnappingSystem: Settings saved to " << filepath << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "GridSnappingSystem: Error saving settings: " << e.what() << std::endl;
        return false;
    }
}

bool GridSnappingSystem::loadSettingsFromFile(const std::string& filepath) {
    try {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            std::cerr << "GridSnappingSystem: Cannot open file for reading: " << filepath << std::endl;
            return false;
        }

        // Простой парсинг JSON (можно заменить на nlohmann::json если нужно)
        std::string line;
        while (std::getline(file, line)) {
            if (line.find("\"enabled\"") != std::string::npos) {
                settings_.enabled = (line.find("true") != std::string::npos);
            } else if (line.find("\"grid_size\"") != std::string::npos) {
                size_t pos = line.find(":");
                if (pos != std::string::npos) {
                    settings_.grid_size = std::stoi(line.substr(pos + 1));
                }
            } else if (line.find("\"snap_position\"") != std::string::npos) {
                settings_.snap_position = (line.find("true") != std::string::npos);
            } else if (line.find("\"snap_size\"") != std::string::npos) {
                settings_.snap_size = (line.find("true") != std::string::npos);
            } else if (line.find("\"show_grid_overlay\"") != std::string::npos) {
                settings_.show_grid_overlay = (line.find("true") != std::string::npos);
            } else if (line.find("\"snap_threshold\"") != std::string::npos) {
                size_t pos = line.find(":");
                if (pos != std::string::npos) {
                    settings_.snap_threshold = std::stof(line.substr(pos + 1));
                }
            } else if (line.find("\"enable_fine_tuning\"") != std::string::npos) {
                settings_.enable_fine_tuning = (line.find("true") != std::string::npos);
            } else if (line.find("\"fine_grid_divisor\"") != std::string::npos) {
                size_t pos = line.find(":");
                if (pos != std::string::npos) {
                    settings_.fine_grid_divisor = std::stoi(line.substr(pos + 1));
                }
            }
        }

        file.close();
        
        // Сброс кэша
        position_cache_.valid = false;
        size_cache_.valid = false;
        
        std::cout << "GridSnappingSystem: Settings loaded from " << filepath << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "GridSnappingSystem: Error loading settings: " << e.what() << std::endl;
        return false;
    }
}

void GridSnappingSystem::resetToDefaults() {
    settings_ = GridSnappingSettings();
    position_cache_.valid = false;
    size_cache_.valid = false;
    std::cout << "GridSnappingSystem: Settings reset to defaults" << std::endl;
}

} // namespace ui
} // namespace llama_gui
