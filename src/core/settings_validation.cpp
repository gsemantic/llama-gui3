#include "../include/core/settings.h"
#include <algorithm>
#include <sstream>

namespace llama_gui {
namespace core {

/**
 * @brief Вычислить минимальный запас на промпт/историю
 * @return Запас в токенах (20% от ctx_size, но не меньше 512)
 */
int Settings::get_prompt_reserve() const {
    int ctx_size = batch_settings_.ctx_size;
    // 20% от ctx_size, но не меньше 512 токенов
    int reserve = std::max(512, static_cast<int>(ctx_size * 0.20f));
    return reserve;
}

/**
 * @brief Вычислить максимально допустимый max_tokens
 * @return Максимально допустимое значение max_tokens
 */
int Settings::get_max_allowed_tokens() const {
    int ctx_size = batch_settings_.ctx_size;
    int reserve = get_prompt_reserve();
    int max_allowed = ctx_size - reserve;
    // Убедимся, что результат неотрицательный
    return std::max(0, max_allowed);
}

/**
 * @brief Вычислить рекомендуемое значение max_tokens
 * @return Рекомендуемое значение (25-30% от ctx_size)
 */
int Settings::get_recommended_max_tokens() const {
    int ctx_size = batch_settings_.ctx_size;
    // Рекомендуется 25% от ctx_size
    int recommended = static_cast<int>(ctx_size * 0.25f);
    // Ограничим сверху максимально допустимым значением
    return std::min(recommended, get_max_allowed_tokens());
}

/**
 * @brief Проверить корректность max_tokens относительно ctx_size
 * @return true если max_tokens в допустимых пределах
 */
bool Settings::validate_max_tokens() const {
    int max_tokens = chat_settings_.max_tokens;
    int max_allowed = get_max_allowed_tokens();
    
    // max_tokens не должен превышать максимально допустимое значение
    return max_tokens <= max_allowed && max_tokens >= 0;
}

/**
 * @brief Получить описание ошибки валидации max_tokens
 * @return Строка с описанием ошибки или пустая строка если ошибок нет
 */
std::string Settings::get_max_tokens_validation_error() const {
    if (validate_max_tokens()) {
        return "";
    }

    std::ostringstream error;
    
    int max_tokens = chat_settings_.max_tokens;
    int ctx_size = batch_settings_.ctx_size;
    int reserve = get_prompt_reserve();
    int max_allowed = get_max_allowed_tokens();
    int recommended = get_recommended_max_tokens();

    error << "Значение max_tokens (" << max_tokens 
          << ") превышает допустимый лимит (" << max_allowed << ").\n"
          << "При контексте " << ctx_size << " токенов:\n"
          << "  - Запас на промпт/историю: " << reserve << " токенов (20%)\n"
          << "  - Максимум для генерации: " << max_allowed << " токенов\n"
          << "  - Рекомендуется: " << recommended << " токенов (25%)";

    return error.str();
}

} // namespace core
} // namespace llama_gui
