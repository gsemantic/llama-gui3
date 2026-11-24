// test_plugin_rag_gate.cpp — R0.2: выключатель RAG для плагинов.
//
// До R0.2 каждый из пяти host_rag_* проверял только наличие rag_manager.
// Пользователь выключал RAG в настройках (settings.rag().enable_rag=false),
// в собственном чате RAG замолкал, а плагин с правом rag_access продолжал
// искать по документам — то есть один переключатель имел два поведения.
//
// Проверяется jedinoe место решения — plugin_rag_allowed():
//   - RAG включён и менеджер есть  → true;
//   - RAG выключен, менеджер есть  → false (главный дефект R0.2);
//   - RAG включён, менеджера нет   → false;
//   - settings == nullptr          → false (усечённый хост/тест);
//   - оба nullptr                  → false.
//
// Тест не грузит плагины и не требует RagManager: plugin_rag_allowed
// читает только указатели, поэтому проверяется на собранных структурах.

#include "../../include/plugins/plugin_manager.h"
#include "../../include/core/settings.h"
#include "../test_framework.h"

#include <memory>
#include <string>

using namespace llama_gui::plugin;
using llama_gui::core::Settings;

namespace {

// Settings с изолированным каталогом профилей: конструктор Settings
// создаёт "profiles" рядом с рабочим каталогом, и в тесте это мусор.
// set_profiles_directory уводит его в /tmp. Settings некопируемый,
// поэтому живёт в unique_ptr и передаётся по указателю.
std::unique_ptr<Settings> make_settings() {
    auto s = std::make_unique<Settings>();
    s->set_profiles_directory("/tmp/opencode/rag_gate_test_profiles");
    return s;
}

} // namespace

void test_rag_allowed_when_enabled_and_manager_present() {
    auto s = make_settings();
    s->rag().enable_rag = true;

    PluginSubsystems subs;
    subs.settings = s.get();
    subs.rag_manager = reinterpret_cast<llama_gui::core::RagManager*>(0x1); // не NULL

    TEST_ASSERT_TRUE(plugin_rag_allowed(subs));
}

void test_rag_denied_when_disabled_but_manager_present() {
    // Главный дефект R0.2: менеджер есть, но пользователь выключил RAG.
    auto s = make_settings();
    s->rag().enable_rag = false;

    PluginSubsystems subs;
    subs.settings = s.get();
    subs.rag_manager = reinterpret_cast<llama_gui::core::RagManager*>(0x1);

    TEST_ASSERT_FALSE(plugin_rag_allowed(subs));
}

void test_rag_denied_when_manager_missing() {
    auto s = make_settings();
    s->rag().enable_rag = true;

    PluginSubsystems subs;
    subs.settings = s.get();
    subs.rag_manager = nullptr;

    TEST_ASSERT_FALSE(plugin_rag_allowed(subs));
}

void test_rag_denied_when_settings_missing() {
    // Усечённый хост (тест плагина без подсистем): «не знаю, включён ли»
    // трактуется как «выключен», а не «разрешаю».
    PluginSubsystems subs;
    subs.settings = nullptr;
    subs.rag_manager = reinterpret_cast<llama_gui::core::RagManager*>(0x1);

    TEST_ASSERT_FALSE(plugin_rag_allowed(subs));
}

void test_rag_denied_when_both_missing() {
    PluginSubsystems subs;
    subs.settings = nullptr;
    subs.rag_manager = nullptr;

    TEST_ASSERT_FALSE(plugin_rag_allowed(subs));
}

void test_rag_denied_by_default_constructed_subsystems() {
    // PluginSubsystems по умолчанию — оба nullptr. Это состояние тестов
    // вроде test_plugin_loader, и оно обязано означать «RAG выключен».
    PluginSubsystems subs;
    TEST_ASSERT_FALSE(plugin_rag_allowed(subs));
}

void test_rag_gate_tracks_enable_rag_changes() {
    // Выключатель живой: PluginManager получает указатель на Settings, и
    // пользователь меняет флаг на ходу (меню настроек). Гейт обязан
    // увидеть изменение без пересоздания subsystems.
    auto s = make_settings();
    PluginSubsystems subs;
    subs.settings = s.get();
    subs.rag_manager = reinterpret_cast<llama_gui::core::RagManager*>(0x1);

    s->rag().enable_rag = true;
    TEST_ASSERT_TRUE(plugin_rag_allowed(subs));

    s->rag().enable_rag = false;
    TEST_ASSERT_FALSE(plugin_rag_allowed(subs));

    s->rag().enable_rag = true;
    TEST_ASSERT_TRUE(plugin_rag_allowed(subs));
}

int main() {
    REGISTER_TEST(test_rag_allowed_when_enabled_and_manager_present);
    REGISTER_TEST(test_rag_denied_when_disabled_but_manager_present);
    REGISTER_TEST(test_rag_denied_when_manager_missing);
    REGISTER_TEST(test_rag_denied_when_settings_missing);
    REGISTER_TEST(test_rag_denied_when_both_missing);
    REGISTER_TEST(test_rag_denied_by_default_constructed_subsystems);
    REGISTER_TEST(test_rag_gate_tracks_enable_rag_changes);

    return test::TestRunner::instance().run();
}