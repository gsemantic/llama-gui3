#include "wp_module.h"
#include "wp_tools.h"
#include "wp_prompts.h"
#include "../../core/engine.h"

namespace coder {
namespace wp {

namespace {

void wp_init() {
    /* Регистрируем WP-инструменты в глобальном реестре. */
    register_wp_tools();
}

std::vector<ToolInfo> wp_get_tools() {
    /* Инструменты уже зарегистрированы в init().
     * Возвращаем пустой вектор — движок берёт из ToolsRegistry. */
    return {};
}

std::vector<Skill> wp_get_skills() {
    return get_wp_skills();
}

const char* wp_get_system_prompt() {
    return kWpSystemPrompt;
}

void wp_render_panel() {
    /* Опциональная UI-панель модуля (пока пусто — рендерится из plugin_main). */
}

void wp_load_settings(const std::string& data_dir) {
    /* Загрузка WP-специфичных настроек. */
}

void wp_save_settings() {
    /* Сохранение WP-специфичных настроек. */
}

void wp_shutdown() {
    /* Очистка ресурсов модуля. */
}

} // anonymous namespace

/* Статическая структура модуля (хранится в статической памяти). */
static const CoderModule s_wp_module = {
    "wordpress",
    "WordPress",
    "Темы, плагины, хуки, WP-CLI, REST API, деплой",
    wp_init,
    wp_get_tools,
    wp_get_skills,
    wp_get_system_prompt,
    wp_render_panel,
    wp_load_settings,
    wp_save_settings,
    wp_shutdown
};

void register_module() {
    ModuleRegistry::instance().register_module(&s_wp_module);
}

} // namespace wp
} // namespace coder
