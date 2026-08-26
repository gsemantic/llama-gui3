#include "../../include/ui/dialog_manager.h"
#include "../test_framework.h"

using namespace llama_gui::ui;

void test_dialog_type_enum() {
    DialogType t1 = DialogType::Help;
    DialogType t2 = DialogType::About;
    DialogType t3 = DialogType::Error;
    DialogType t4 = DialogType::Documentation;

    TEST_ASSERT(t1 != t2);
    TEST_ASSERT(t3 != t4);
}

void test_dialog_struct_defaults() {
    Dialog dialog;

    TEST_ASSERT(dialog.visible == false);
    TEST_ASSERT(dialog.modal == true);
    TEST_ASSERT(dialog.position.x == 0.0f);
    TEST_ASSERT(dialog.position.y == 0.0f);
    TEST_ASSERT(dialog.size.x == 400.0f);
    TEST_ASSERT(dialog.size.y == 300.0f);
}

void test_dialog_button() {
    Dialog::Button button;
    button.text = "OK";
    button.is_default = true;
    button.is_cancel = false;

    TEST_ASSERT_EQUAL(button.text, std::string("OK"));
    TEST_ASSERT_TRUE(button.is_default);
    TEST_ASSERT_FALSE(button.is_cancel);
}

int main() {
    REGISTER_TEST(test_dialog_type_enum);
    REGISTER_TEST(test_dialog_struct_defaults);
    REGISTER_TEST(test_dialog_button);

    return test::TestRunner::instance().run();
}
