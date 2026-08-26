#include "../../include/ui/input_handler.h"
#include "../test_framework.h"

using namespace llama_gui::ui;

void test_input_handler_creation() {
    InputHandler handler;
    TEST_ASSERT_TRUE(handler.has_focus());
}

void test_shortcut_registration() {
    InputHandler handler;
    handler.initialize();

    bool called = false;
    handler.registerShortcut("test", 97, 0, [&called]() { called = true; });
    handler.setShortcutCallback([](const std::string&) {});
    handler.handleShortcut(97, 0);

    TEST_ASSERT_TRUE(called);
}

void test_shortcut_wrong_key() {
    InputHandler handler;
    handler.initialize();

    bool called = false;
    handler.registerShortcut("test", 97, 0, [&called]() { called = true; });
    handler.setShortcutCallback([](const std::string&) {});
    handler.handleShortcut(98, 0);

    TEST_ASSERT_FALSE(called);
}

void test_window_toggle() {
    InputHandler handler;
    handler.initialize();

    std::string toggled;
    handler.setShortcutCallback([&toggled](const std::string& w) { toggled = w; });
    handler.registerWindowToggleShortcut("chat", 282, 0);
    handler.handleShortcut(282, 0);

    TEST_ASSERT_EQUAL(toggled, std::string("chat"));
}

void test_key_callback() {
    InputHandler handler;
    handler.initialize();

    int captured = 0;
    handler.setKeyDownCallback([&captured](int key, int) { captured = key; });
    handler.handleKeyDown(42, 0);

    TEST_ASSERT_EQUAL(captured, 42);
}

int main() {
    REGISTER_TEST(test_input_handler_creation);
    REGISTER_TEST(test_shortcut_registration);
    REGISTER_TEST(test_shortcut_wrong_key);
    REGISTER_TEST(test_window_toggle);
    REGISTER_TEST(test_key_callback);

    return test::TestRunner::instance().run();
}
